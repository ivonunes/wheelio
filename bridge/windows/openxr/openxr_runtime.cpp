// Wheelio OpenXR runtime, Phase 0.
//
// A minimal OpenXR runtime loaded by the game's own OpenXR loader. It exposes
// one stereo view configuration, LOCAL/STAGE/VIEW reference spaces,
// XR_KHR_D3D11_enable or XR_KHR_D3D12_enable swapchains backed by the game's
// own device and a paced frame loop driven by the headset's head pose. Input is present but inactive. On chosen
// frames xrEndFrame reads both eye textures back and writes them as PNG next
// to the DLL so we can prove the game renders through us.
//
// Everything the game calls is logged. Anything unimplemented logs and returns
// XR_ERROR_FUNCTION_UNSUPPORTED so gaps show up instead of being papered over.

#include "frame_export.hpp"
#include "png_writer.hpp"
#include "runtime_log.hpp"
#include "xr_math.hpp"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <shlobj.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr char kRuntimeName[] = "Wheelio OpenXR Runtime";
constexpr XrVersion kRuntimeVersion = XR_MAKE_VERSION(0, 1, 0);
constexpr char kSystemName[] = "Wheelio Quest 3 Bridge";

// Per-eye size, matching what the streamer negotiates with the headset (ALVR
// aligns the stream down to multiples of 32) so frames go to the encoder
// without scaling. Override with WHEELIO_XR_VIEW_SIZE=WxH for measurement runs.
constexpr uint32_t kDefaultViewWidth = 2048;
constexpr uint32_t kDefaultViewHeight = 2208;
constexpr uint32_t kMaxViewSize = 8192;

constexpr double kDisplayHz = 72.0;
constexpr XrDuration kFramePeriodNs = static_cast<XrDuration>(1000000000.0 / kDisplayHz);

// Quest 3 optics as reported by its ALVR client, used until the streamer has
// saved the connected headset's own values (see read_headset_optics). The
// game reads the FOV once, on its first frames, and we declare it immutable,
// so these must be right before the headset connects: ALVR's client maps every
// frame assuming the server rendered with the headset's exact FOV, and any
// difference shows as the image swimming with head movement.
// Left eye; the right eye mirrors the horizontal angles.
constexpr XrFovf kLeftEyeFov{-0.94247776f, 0.6981317f, 0.7679449f, -0.9599311f};
constexpr float kInterpupillaryDistance = 0.0631f;

// LOCAL space origin is where the head was at start; STAGE origin is on the
// floor. With a fixed pose, the seated head sits this far above the floor.
constexpr float kSeatedEyeHeight = 1.3f;

// Frames at which xrEndFrame writes both eye textures to PNG, when
// WHEELIO_XR_DUMP=1 is set. Off by default: each pair costs 28 MB on disk.
constexpr uint64_t kDumpFrames[] = {120, 600, 1800};
bool g_dump_frames_enabled = false;

constexpr uint32_t kMaxSwapchains = 16;
constexpr uint32_t kSwapchainImageCount = 3;
constexpr uint32_t kMaxSpaces = 64;
constexpr uint32_t kMaxActionSets = 16;
constexpr uint32_t kMaxActions = 256;
constexpr uint32_t kMaxPaths = 512;
constexpr uint32_t kMaxQueuedEvents = 32;

// Per-frame entry points log their first calls, then periodically.
constexpr uint64_t kVerboseCalls = 5;
constexpr uint64_t kPeriodicLogEvery = 1000;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Instance {
    bool alive = false;
    XrVersion api_version = 0;
    char application_name[XR_MAX_APPLICATION_NAME_SIZE] = {};
};

struct Space {
    bool alive = false;
    bool is_action_space = false;
    XrReferenceSpaceType reference_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrPosef pose_in_reference = pose_identity();
};

struct Swapchain {
    bool alive = false;
    XrSwapchainCreateInfo info{};
    // One set is populated, matching the session's graphics API.
    ID3D11Texture2D* images[kSwapchainImageCount] = {};
    ID3D12Resource* images12[kSwapchainImageCount] = {};
    // Index handed out by xrAcquireSwapchainImage and not yet released, or -1.
    int acquired = -1;
    // Index the app most recently released: what xrEndFrame composites.
    int last_released = -1;
    // Rotating acquisition cursor.
    uint32_t next = 0;
};

struct OpaqueHandle {
    bool alive = false;
    char name[XR_MAX_ACTION_NAME_SIZE] = {};
};

enum class GraphicsApi { none, d3d11, d3d12 };

struct Session {
    bool alive = false;
    GraphicsApi api = GraphicsApi::none;
    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue12 = nullptr;
    // Copies eye textures out of whichever device the game bound.
    GpuReadback* readback = nullptr;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exit_requested = false;

    XrTime last_predicted_display = 0;
    bool frame_begun = false;
    uint64_t wait_count = 0;
    uint64_t begin_count = 0;
    uint64_t end_count = 0;
    uint64_t locate_views_count = 0;
    uint64_t acquire_count = 0;
    uint64_t sync_actions_count = 0;
};

Instance g_instance;
Session g_session;
Space g_spaces[kMaxSpaces];
Swapchain g_swapchains[kMaxSwapchains];
OpaqueHandle g_action_sets[kMaxActionSets];
OpaqueHandle g_actions[kMaxActions];
char g_paths[kMaxPaths][XR_MAX_PATH_LENGTH];
uint32_t g_path_count = 0;
XrEventDataBuffer g_events[kMaxQueuedEvents];
uint32_t g_event_head = 0;
uint32_t g_event_count = 0;

// Head pose in the world (STAGE) frame, from the headset via the mac app. Until
// the first pose arrives the head sits at the seated height, looking forward.
XrPosef g_head_pose = pose_identity();
bool g_have_tracking = false;
// Where LOCAL's origin sits in the world: the head pose when tracking first
// arrived, moved again whenever the headset recentres.
XrPosef g_local_origin = pose_identity();
uint32_t g_recenter_generation = 0;
// Optics from the headset, replacing the placeholders once known.
XrFovf g_fov[2] = {kLeftEyeFov, {-kLeftEyeFov.angleRight, -kLeftEyeFov.angleLeft, kLeftEyeFov.angleUp, kLeftEyeFov.angleDown}};
float g_ipd = kInterpupillaryDistance;
bool g_views_checked = false;
// The tracking sample the most recent xrLocateViews answered with, keyed by the
// display time the game asked about, so xrEndFrame can find which sample a
// frame was rendered from even with a frame in flight.
struct LocatedFrame {
    XrTime display_time = 0;
    uint64_t tracking_timestamp_ns = 0;
    XrPosef head = pose_identity();  // world-frame head pose answered for this display time
};
LocatedFrame g_located[8];
uint32_t g_located_next = 0;
uint64_t g_last_tracking_timestamp_ns = 0;
uint64_t g_unmatched_frames = 0;

uint32_t g_view_width = kDefaultViewWidth;
uint32_t g_view_height = kDefaultViewHeight;

// Publishes the rendered eyes to the mac app. Started lazily on the first
// xrEndFrame, once we know the swapchain size and format the game settled on.
FrameExporter g_frame_exporter;

CRITICAL_SECTION g_lock;
LARGE_INTEGER g_qpc_frequency{};
HMODULE g_module = nullptr;

struct ScopedLock {
    ScopedLock() { EnterCriticalSection(&g_lock); }
    ~ScopedLock() { LeaveCriticalSection(&g_lock); }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

XrTime now_ns() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // Split to avoid overflowing 64 bits on long uptimes.
    const int64_t seconds = counter.QuadPart / g_qpc_frequency.QuadPart;
    const int64_t remainder = counter.QuadPart % g_qpc_frequency.QuadPart;
    return seconds * 1000000000LL + (remainder * 1000000000LL) / g_qpc_frequency.QuadPart;
}

void sleep_until(XrTime target) {
    for (;;) {
        const XrTime remaining = target - now_ns();
        if (remaining <= 0) {
            return;
        }
        if (remaining > 2000000) {
            Sleep(static_cast<DWORD>((remaining - 1000000) / 1000000));
        } else {
            SwitchToThread();
        }
    }
}

bool should_log_call(uint64_t count) {
    return count <= kVerboseCalls || (count % kPeriodicLogEvery) == 0;
}

const char* result_name(XrResult result);
const char* structure_name(XrStructureType type);
const char* session_state_name(XrSessionState state);
const char* reference_space_name(XrReferenceSpaceType type);

void log_next_chain(const char* owner, const void* next) {
    const auto* header = static_cast<const XrBaseInStructure*>(next);
    while (header) {
        runtime_logf("  %s next chain: %s (%d)", owner, structure_name(header->type), static_cast<int>(header->type));
        header = header->next;
    }
}

void queue_session_state(XrSessionState state) {
    if (g_event_count == kMaxQueuedEvents) {
        runtime_log("event queue full, dropping session state event");
        return;
    }
    XrEventDataBuffer& slot = g_events[(g_event_head + g_event_count) % kMaxQueuedEvents];
    std::memset(&slot, 0, sizeof(slot));
    auto* event = reinterpret_cast<XrEventDataSessionStateChanged*>(&slot);
    event->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    event->session = reinterpret_cast<XrSession>(&g_session);
    event->state = state;
    event->time = now_ns();
    ++g_event_count;
    g_session.state = state;
    runtime_logf("  queued session state -> %s", session_state_name(state));
}

// Pulls the newest head pose and optics from the mac app. Called with the
// lock held, from the frame loop entry points.
void refresh_tracking() {
    wheelio_vr::HostState state{};
    if (!g_frame_exporter.read_host_state(state)) {
        return;
    }
    if (state.has_views && !g_views_checked) {
        // The FOV is immutable for the session (the game caches it), so only
        // compare: a difference means the saved optics were stale and the next
        // game launch will be right.
        g_views_checked = true;
        const float difference = std::fabs(state.fov[0][0] - g_fov[0].angleLeft) +
                                 std::fabs(state.fov[0][1] - g_fov[0].angleRight) +
                                 std::fabs(state.fov[0][2] - g_fov[0].angleUp) +
                                 std::fabs(state.fov[0][3] - g_fov[0].angleDown);
        if (difference > 0.001f) {
            runtime_logf("headset FOV (%.3f %.3f %.3f %.3f) differs from the FOV this session renders with; "
                         "restart the game to pick up the saved optics",
                         state.fov[0][0], state.fov[0][1], state.fov[0][2], state.fov[0][3]);
        }
        if (state.ipd_m > 0.04f && state.ipd_m < 0.09f) {
            g_ipd = state.ipd_m;
        }
    }
    if (state.has_pose == 0.0f) {
        return;
    }
    g_head_pose.orientation = XrQuaternionf{state.head_orientation[0], state.head_orientation[1],
                                            state.head_orientation[2], state.head_orientation[3]};
    g_head_pose.position = XrVector3f{state.head_position[0], state.head_position[1], state.head_position[2]};

    // LOCAL's origin sits at the head, facing the way the head faces (yaw
    // only, so the horizon stays level). Whatever ALVR's own recentring left
    // as "forward", the game's forward is where the user is looking.
    if (!g_have_tracking || state.recenter_generation != g_recenter_generation) {
        XrQuaternionf yaw = g_head_pose.orientation;
        yaw.x = 0.0f;
        yaw.z = 0.0f;
        const float length = std::sqrt(yaw.y * yaw.y + yaw.w * yaw.w);
        if (length > 0.0001f) {
            yaw.y /= length;
            yaw.w /= length;
        } else {
            yaw = quat_identity();
        }
        g_local_origin = XrPosef{yaw, g_head_pose.position};
        g_recenter_generation = state.recenter_generation;
        runtime_logf("tracking: LOCAL origin set at (%.2f %.2f %.2f) yaw quat (%.3f %.3f), recentre generation %u",
                     g_local_origin.position.x, g_local_origin.position.y, g_local_origin.position.z, yaw.y, yaw.w,
                     g_recenter_generation);
    }
    g_have_tracking = true;
    g_last_tracking_timestamp_ns = state.tracking_timestamp_ns;
}

// Pose of a space's origin in the world frame (world = STAGE, ALVR's floor
// origin once tracking is live).
XrPosef space_origin_in_world(const Space& space) {
    XrPosef local_origin = g_local_origin;
    XrPosef head = g_head_pose;
    if (!g_have_tracking) {
        local_origin.position.y = kSeatedEyeHeight;
        head = pose_multiply(local_origin, g_head_pose);
    }

    XrPosef reference = pose_identity();
    switch (space.reference_type) {
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
        reference = local_origin;
        reference.position.y = 0.0f;
        break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        reference = local_origin;
        break;
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        reference = head;
        break;
    default:
        break;
    }
    return pose_multiply(reference, space.pose_in_reference);
}

template <typename T, uint32_t N>
T* allocate_slot(T (&pool)[N]) {
    for (uint32_t i = 0; i < N; ++i) {
        if (!pool[i].alive) {
            pool[i] = T{};
            pool[i].alive = true;
            return &pool[i];
        }
    }
    return nullptr;
}

template <typename T, uint32_t N>
bool is_live_handle(T (&pool)[N], const void* handle) {
    const auto* candidate = static_cast<const T*>(handle);
    return candidate >= &pool[0] && candidate < &pool[N] && candidate->alive;
}

bool check_instance(XrInstance instance) {
    return reinterpret_cast<Instance*>(instance) == &g_instance && g_instance.alive;
}

bool check_session(XrSession session) {
    return reinterpret_cast<Session*>(session) == &g_session && g_session.alive;
}

void copy_string(char* destination, size_t destination_size, const char* source) {
    std::snprintf(destination, destination_size, "%s", source ? source : "");
}

// Two-call idiom: fill `count_output`, then copy when capacity allows.
template <typename T>
XrResult return_array(uint32_t capacity, uint32_t* count_output, T* out, const T* values, uint32_t count) {
    if (!count_output) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *count_output = count;
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < count || !out) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = values[i];
    }
    return XR_SUCCESS;
}

void read_view_size_override() {
    char value[64] = {};
    if (GetEnvironmentVariableA("WHEELIO_XR_VIEW_SIZE", value, sizeof(value)) == 0) {
        return;
    }
    unsigned width = 0;
    unsigned height = 0;
    if (std::sscanf(value, "%ux%u", &width, &height) == 2 && width > 0 && height > 0 && width <= kMaxViewSize &&
        height <= kMaxViewSize) {
        g_view_width = width;
        g_view_height = height;
        runtime_logf("view size override: %ux%u", width, height);
    } else {
        runtime_logf("ignoring malformed WHEELIO_XR_VIEW_SIZE=%s", value);
    }
}

// The streamer writes %LOCALAPPDATA%\Wheelio\headset_optics.txt whenever a
// headset connects: two lines of per-eye FOV (left right up down, radians)
// and one with the IPD in metres. Read once per instance.
void read_headset_optics() {
    char local_appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_appdata))) {
        return;
    }
    char path[MAX_PATH] = {};
    std::snprintf(path, sizeof(path), "%s\\Wheelio\\headset_optics.txt", local_appdata);
    FILE* file = std::fopen(path, "r");
    if (!file) {
        runtime_log("no saved headset optics; using Quest 3 defaults");
        return;
    }
    XrFovf fov[2];
    float ipd = 0.0f;
    const int read = std::fscanf(file, "%f %f %f %f\n%f %f %f %f\n%f", &fov[0].angleLeft, &fov[0].angleRight,
                                 &fov[0].angleUp, &fov[0].angleDown, &fov[1].angleLeft, &fov[1].angleRight,
                                 &fov[1].angleUp, &fov[1].angleDown, &ipd);
    std::fclose(file);
    if (read != 9 || ipd < 0.04f || ipd > 0.09f) {
        runtime_logf("ignoring malformed %s", path);
        return;
    }
    g_fov[0] = fov[0];
    g_fov[1] = fov[1];
    g_ipd = ipd;
    runtime_logf("headset optics: left eye fov (%.3f %.3f %.3f %.3f), ipd %.1f mm", fov[0].angleLeft,
                 fov[0].angleRight, fov[0].angleUp, fov[0].angleDown, ipd * 1000.0f);
}

void read_dump_frames_override() {
    char value[16] = {};
    g_dump_frames_enabled = GetEnvironmentVariableA("WHEELIO_XR_DUMP", value, sizeof(value)) != 0 &&
                            value[0] == '1';
    if (g_dump_frames_enabled) {
        runtime_log("PNG frame dumps enabled");
    }
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

const XrExtensionProperties kSupportedExtensions[] = {
    {XR_TYPE_EXTENSION_PROPERTIES, nullptr, XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D11_enable_SPEC_VERSION},
    {XR_TYPE_EXTENSION_PROPERTIES, nullptr, XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_enable_SPEC_VERSION},
};

XrResult XRAPI_CALL rt_xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t propertyCapacityInput,
                                                             uint32_t* propertyCountOutput,
                                                             XrExtensionProperties* properties) {
    runtime_logf("xrEnumerateInstanceExtensionProperties layer=%s capacity=%u", layerName ? layerName : "(null)",
                 propertyCapacityInput);
    if (layerName) {
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }
    const uint32_t count = sizeof(kSupportedExtensions) / sizeof(kSupportedExtensions[0]);
    if (!propertyCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *propertyCountOutput = count;
    if (propertyCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (propertyCapacityInput < count || !properties) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
        copy_string(properties[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, kSupportedExtensions[i].extensionName);
        properties[i].extensionVersion = kSupportedExtensions[i].extensionVersion;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput, uint32_t* propertyCountOutput,
                                                    XrApiLayerProperties* properties) {
    (void)propertyCapacityInput;
    (void)properties;
    runtime_log("xrEnumerateApiLayerProperties");
    if (!propertyCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *propertyCountOutput = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
    ScopedLock lock;
    if (!createInfo || !instance) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const XrApplicationInfo& app = createInfo->applicationInfo;
    runtime_logf("xrCreateInstance app=\"%s\" v%u engine=\"%s\" v%u api=%u.%u.%u flags=0x%llx", app.applicationName,
                 app.applicationVersion, app.engineName, app.engineVersion, XR_VERSION_MAJOR(app.apiVersion),
                 XR_VERSION_MINOR(app.apiVersion), XR_VERSION_PATCH(app.apiVersion),
                 static_cast<unsigned long long>(createInfo->createFlags));
    for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; ++i) {
        runtime_logf("  requested api layer: %s", createInfo->enabledApiLayerNames[i]);
    }
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        const char* name = createInfo->enabledExtensionNames[i];
        bool supported = false;
        for (const XrExtensionProperties& ext : kSupportedExtensions) {
            supported = supported || std::strcmp(ext.extensionName, name) == 0;
        }
        runtime_logf("  requested extension: %s (%s)", name, supported ? "supported" : "NOT SUPPORTED");
        if (!supported) {
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    log_next_chain("xrCreateInstance", createInfo->next);

    if (g_instance.alive) {
        runtime_log("  xrCreateInstance: instance already exists");
        return XR_ERROR_LIMIT_REACHED;
    }
    if (XR_VERSION_MAJOR(app.apiVersion) != 1) {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }

    g_instance = Instance{};
    g_instance.alive = true;
    g_instance.api_version = app.apiVersion;
    copy_string(g_instance.application_name, sizeof(g_instance.application_name), app.applicationName);
    g_path_count = 0;
    read_view_size_override();
    read_dump_frames_override();
    read_headset_optics();

    *instance = reinterpret_cast<XrInstance>(&g_instance);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrDestroyInstance(XrInstance instance) {
    ScopedLock lock;
    runtime_log("xrDestroyInstance");
    if (!check_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    g_instance.alive = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* instanceProperties) {
    runtime_log("xrGetInstanceProperties");
    if (!check_instance(instance) || !instanceProperties) {
        return XR_ERROR_HANDLE_INVALID;
    }
    instanceProperties->runtimeVersion = kRuntimeVersion;
    copy_string(instanceProperties->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, kRuntimeName);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    ScopedLock lock;
    if (!check_instance(instance) || !eventData) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_event_count == 0) {
        return XR_EVENT_UNAVAILABLE;
    }
    *eventData = g_events[g_event_head];
    g_event_head = (g_event_head + 1) % kMaxQueuedEvents;
    --g_event_count;
    if (eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        runtime_logf("xrPollEvent -> session state %s",
                     session_state_name(reinterpret_cast<XrEventDataSessionStateChanged*>(eventData)->state));
    } else {
        runtime_logf("xrPollEvent -> %s", structure_name(eventData->type));
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    (void)instance;
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", result_name(value));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrStructureTypeToString(XrInstance instance, XrStructureType value,
                                              char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    (void)instance;
    std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", structure_name(value));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrStringToPath(XrInstance instance, const char* pathString, XrPath* path) {
    ScopedLock lock;
    if (!check_instance(instance) || !pathString || !path) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (uint32_t i = 0; i < g_path_count; ++i) {
        if (std::strcmp(g_paths[i], pathString) == 0) {
            *path = i + 1;
            return XR_SUCCESS;
        }
    }
    if (g_path_count == kMaxPaths || std::strlen(pathString) >= XR_MAX_PATH_LENGTH) {
        runtime_logf("xrStringToPath: cannot intern \"%s\"", pathString);
        return XR_ERROR_PATH_COUNT_EXCEEDED;
    }
    copy_string(g_paths[g_path_count], XR_MAX_PATH_LENGTH, pathString);
    ++g_path_count;
    *path = g_path_count;
    runtime_logf("xrStringToPath \"%s\" -> %llu", pathString, static_cast<unsigned long long>(*path));
    return XR_SUCCESS;
}

const char* path_string(XrPath path) {
    if (path == XR_NULL_PATH || path > g_path_count) {
        return "(null path)";
    }
    return g_paths[path - 1];
}

XrResult XRAPI_CALL rt_xrPathToString(XrInstance instance, XrPath path, uint32_t bufferCapacityInput,
                                     uint32_t* bufferCountOutput, char* buffer) {
    ScopedLock lock;
    if (!check_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (path == XR_NULL_PATH || path > g_path_count) {
        return XR_ERROR_PATH_INVALID;
    }
    const char* value = g_paths[path - 1];
    const uint32_t needed = static_cast<uint32_t>(std::strlen(value)) + 1;
    if (!bufferCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *bufferCountOutput = needed;
    if (bufferCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (bufferCapacityInput < needed || !buffer) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, value, needed);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

constexpr XrSystemId kSystemId = 1;

XrResult XRAPI_CALL rt_xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
    if (!check_instance(instance) || !getInfo || !systemId) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrGetSystem formFactor=%d", static_cast<int>(getInfo->formFactor));
    log_next_chain("xrGetSystem", getInfo->next);
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
    *systemId = kSystemId;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetSystemProperties(XrInstance instance, XrSystemId systemId,
                                            XrSystemProperties* properties) {
    runtime_log("xrGetSystemProperties");
    if (!check_instance(instance) || !properties) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    log_next_chain("xrGetSystemProperties", properties->next);
    properties->systemId = kSystemId;
    properties->vendorId = 0;
    copy_string(properties->systemName, XR_MAX_SYSTEM_NAME_SIZE, kSystemName);
    properties->graphicsProperties.maxSwapchainImageWidth = kMaxViewSize;
    properties->graphicsProperties.maxSwapchainImageHeight = kMaxViewSize;
    properties->graphicsProperties.maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                       XrViewConfigurationType viewConfigurationType,
                                                       uint32_t environmentBlendModeCapacityInput,
                                                       uint32_t* environmentBlendModeCountOutput,
                                                       XrEnvironmentBlendMode* environmentBlendModes) {
    runtime_logf("xrEnumerateEnvironmentBlendModes viewConfig=%d", static_cast<int>(viewConfigurationType));
    if (!check_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    const XrEnvironmentBlendMode modes[] = {XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    return return_array(environmentBlendModeCapacityInput, environmentBlendModeCountOutput, environmentBlendModes,
                        modes, 1);
}

XrResult XRAPI_CALL rt_xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                                    uint32_t viewConfigurationTypeCapacityInput,
                                                    uint32_t* viewConfigurationTypeCountOutput,
                                                    XrViewConfigurationType* viewConfigurationTypes) {
    runtime_log("xrEnumerateViewConfigurations");
    if (!check_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    const XrViewConfigurationType types[] = {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    return return_array(viewConfigurationTypeCapacityInput, viewConfigurationTypeCountOutput,
                        viewConfigurationTypes, types, 1);
}

XrResult XRAPI_CALL rt_xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId,
                                                       XrViewConfigurationType viewConfigurationType,
                                                       XrViewConfigurationProperties* configurationProperties) {
    runtime_logf("xrGetViewConfigurationProperties viewConfig=%d", static_cast<int>(viewConfigurationType));
    if (!check_instance(instance) || !configurationProperties) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    configurationProperties->viewConfigurationType = viewConfigurationType;
    configurationProperties->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                        XrViewConfigurationType viewConfigurationType,
                                                        uint32_t viewCapacityInput, uint32_t* viewCountOutput,
                                                        XrViewConfigurationView* views) {
    runtime_logf("xrEnumerateViewConfigurationViews viewConfig=%d capacity=%u", static_cast<int>(viewConfigurationType),
                 viewCapacityInput);
    if (!check_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (!viewCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *viewCountOutput = 2;
    if (viewCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (viewCapacityInput < 2 || !views) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        views[i].recommendedImageRectWidth = g_view_width;
        views[i].maxImageRectWidth = kMaxViewSize;
        views[i].recommendedImageRectHeight = g_view_height;
        views[i].maxImageRectHeight = kMaxViewSize;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    runtime_logf("  recommended view %ux%u", g_view_width, g_view_height);
    return XR_SUCCESS;
}

// The first adapter: the game must create its device on it.
LUID first_adapter_luid() {
    LUID luid{};
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            luid = desc.AdapterLuid;
            runtime_logf("  adapter 0: \"%ls\" vendor=0x%04x device=0x%04x luid=%08lx:%08lx", desc.Description,
                         desc.VendorId, desc.DeviceId, static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                         static_cast<unsigned long>(desc.AdapterLuid.LowPart));
            adapter->Release();
        } else {
            runtime_log("  EnumAdapters1(0) failed");
        }
        factory->Release();
    } else {
        runtime_log("  CreateDXGIFactory1 failed");
    }
    return luid;
}

XrResult XRAPI_CALL rt_xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                                        XrGraphicsRequirementsD3D11KHR* graphicsRequirements) {
    runtime_log("xrGetD3D11GraphicsRequirementsKHR");
    if (!check_instance(instance) || !graphicsRequirements) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    graphicsRequirements->adapterLuid = first_adapter_luid();
    graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetD3D12GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                                        XrGraphicsRequirementsD3D12KHR* graphicsRequirements) {
    runtime_log("xrGetD3D12GraphicsRequirementsKHR");
    if (!check_instance(instance) || !graphicsRequirements) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    graphicsRequirements->adapterLuid = first_adapter_luid();
    graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void release_session_devices() {
    if (g_session.readback) {
        g_session.readback->~GpuReadback();
        g_session.readback = nullptr;
    }
    if (g_session.device) {
        g_session.device->Release();
        g_session.device = nullptr;
    }
    if (g_session.queue12) {
        g_session.queue12->Release();
        g_session.queue12 = nullptr;
    }
    if (g_session.device12) {
        g_session.device12->Release();
        g_session.device12 = nullptr;
    }
}

XrResult XRAPI_CALL rt_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                      XrSession* session) {
    ScopedLock lock;
    runtime_log("xrCreateSession");
    if (!check_instance(instance) || !createInfo || !session) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo->systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    log_next_chain("xrCreateSession", createInfo->next);
    if (g_session.alive) {
        return XR_ERROR_LIMIT_REACHED;
    }

    const XrGraphicsBindingD3D11KHR* binding11 = nullptr;
    const XrGraphicsBindingD3D12KHR* binding12 = nullptr;
    for (const auto* header = static_cast<const XrBaseInStructure*>(createInfo->next); header;
         header = header->next) {
        if (header->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            binding11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(header);
        } else if (header->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            binding12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(header);
        }
    }

    g_session = Session{};
    if (binding11 && binding11->device) {
        g_session.api = GraphicsApi::d3d11;
        g_session.device = binding11->device;
        g_session.device->AddRef();
        g_session.readback = create_d3d11_readback(g_session.device);

        // Which D3D11 implementation is under us: D3DMetal, DXMT or DXVK show
        // up in the adapter description under Wine.
        IDXGIDevice* dxgi_device = nullptr;
        if (SUCCEEDED(g_session.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                adapter->GetDesc(&desc);
                runtime_logf("  game D3D11 device adapter: \"%ls\" vendor=0x%04x feature level=0x%x", desc.Description,
                             desc.VendorId, static_cast<unsigned>(g_session.device->GetFeatureLevel()));
                adapter->Release();
            }
            dxgi_device->Release();
        }
    } else if (binding12 && binding12->device && binding12->queue) {
        g_session.api = GraphicsApi::d3d12;
        g_session.device12 = binding12->device;
        g_session.device12->AddRef();
        g_session.queue12 = binding12->queue;
        g_session.queue12->AddRef();
        g_session.readback = create_d3d12_readback(g_session.device12, g_session.queue12);
        const D3D12_COMMAND_QUEUE_DESC queue = g_session.queue12->GetDesc();
        const LUID luid = g_session.device12->GetAdapterLuid();
        runtime_logf("  game D3D12 device on adapter luid=%08lx:%08lx, queue type %d", static_cast<unsigned long>(luid.HighPart),
                     static_cast<unsigned long>(luid.LowPart), static_cast<int>(queue.Type));
        if (queue.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            runtime_log("  queue is not a direct queue; readback copies will fail");
        }
    } else {
        runtime_log("  no D3D11 or D3D12 graphics binding in chain");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (!g_session.readback) {
        runtime_log("  could not create the readback for this device");
        release_session_devices();
        g_session = Session{};
        return XR_ERROR_RUNTIME_FAILURE;
    }
    g_session.alive = true;

    *session = reinterpret_cast<XrSession>(&g_session);
    queue_session_state(XR_SESSION_STATE_IDLE);
    queue_session_state(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

void destroy_swapchain_locked(Swapchain& swapchain);

XrResult XRAPI_CALL rt_xrDestroySession(XrSession session) {
    ScopedLock lock;
    runtime_log("xrDestroySession");
    if (!check_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    g_frame_exporter.stop();
    for (Swapchain& swapchain : g_swapchains) {
        if (swapchain.alive) {
            destroy_swapchain_locked(swapchain);
        }
    }
    for (Space& space : g_spaces) {
        space.alive = false;
    }
    release_session_devices();
    g_session = Session{};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) {
    ScopedLock lock;
    if (!check_session(session) || !beginInfo) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrBeginSession viewConfig=%d", static_cast<int>(beginInfo->primaryViewConfigurationType));
    log_next_chain("xrBeginSession", beginInfo->next);
    if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (g_session.running) {
        return XR_ERROR_SESSION_RUNNING;
    }
    if (g_session.state != XR_SESSION_STATE_READY) {
        return XR_ERROR_SESSION_NOT_READY;
    }
    g_session.running = true;
    queue_session_state(XR_SESSION_STATE_SYNCHRONIZED);
    queue_session_state(XR_SESSION_STATE_VISIBLE);
    queue_session_state(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrEndSession(XrSession session) {
    ScopedLock lock;
    runtime_log("xrEndSession");
    if (!check_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!g_session.running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (g_session.state != XR_SESSION_STATE_STOPPING) {
        return XR_ERROR_SESSION_NOT_STOPPING;
    }
    g_session.running = false;
    queue_session_state(XR_SESSION_STATE_IDLE);
    queue_session_state(XR_SESSION_STATE_EXITING);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrRequestExitSession(XrSession session) {
    ScopedLock lock;
    runtime_log("xrRequestExitSession");
    if (!check_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!g_session.running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    g_session.exit_requested = true;
    queue_session_state(XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Spaces
// ---------------------------------------------------------------------------

const XrReferenceSpaceType kReferenceSpaces[] = {
    XR_REFERENCE_SPACE_TYPE_VIEW,
    XR_REFERENCE_SPACE_TYPE_LOCAL,
    XR_REFERENCE_SPACE_TYPE_STAGE,
};

XrResult XRAPI_CALL rt_xrEnumerateReferenceSpaces(XrSession session, uint32_t spaceCapacityInput,
                                                 uint32_t* spaceCountOutput, XrReferenceSpaceType* spaces) {
    runtime_log("xrEnumerateReferenceSpaces");
    if (!check_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    return return_array(spaceCapacityInput, spaceCountOutput, spaces, kReferenceSpaces,
                        sizeof(kReferenceSpaces) / sizeof(kReferenceSpaces[0]));
}

XrResult XRAPI_CALL rt_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo,
                                             XrSpace* space) {
    ScopedLock lock;
    if (!check_session(session) || !createInfo || !space) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrCreateReferenceSpace type=%s offset=(%.3f %.3f %.3f)",
                 reference_space_name(createInfo->referenceSpaceType), createInfo->poseInReferenceSpace.position.x,
                 createInfo->poseInReferenceSpace.position.y, createInfo->poseInReferenceSpace.position.z);
    bool supported = false;
    for (XrReferenceSpaceType type : kReferenceSpaces) {
        supported = supported || type == createInfo->referenceSpaceType;
    }
    if (!supported) {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    Space* slot = allocate_slot(g_spaces);
    if (!slot) {
        return XR_ERROR_LIMIT_REACHED;
    }
    slot->reference_type = createInfo->referenceSpaceType;
    slot->pose_in_reference = createInfo->poseInReferenceSpace;
    *space = reinterpret_cast<XrSpace>(slot);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType,
                                                    XrExtent2Df* bounds) {
    runtime_logf("xrGetReferenceSpaceBoundsRect type=%s", reference_space_name(referenceSpaceType));
    if (!check_session(session) || !bounds) {
        return XR_ERROR_HANDLE_INVALID;
    }
    bounds->width = 0.0f;
    bounds->height = 0.0f;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XrResult XRAPI_CALL rt_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo,
                                          XrSpace* space) {
    ScopedLock lock;
    if (!check_session(session) || !createInfo || !space) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrCreateActionSpace subaction=%s", path_string(createInfo->subactionPath));
    Space* slot = allocate_slot(g_spaces);
    if (!slot) {
        return XR_ERROR_LIMIT_REACHED;
    }
    slot->is_action_space = true;
    slot->pose_in_reference = createInfo->poseInActionSpace;
    *space = reinterpret_cast<XrSpace>(slot);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrDestroySpace(XrSpace space) {
    ScopedLock lock;
    runtime_log("xrDestroySpace");
    if (!is_live_handle(g_spaces, space)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    reinterpret_cast<Space*>(space)->alive = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    ScopedLock lock;
    (void)time;
    if (!is_live_handle(g_spaces, space) || !is_live_handle(g_spaces, baseSpace) || !location) {
        return XR_ERROR_HANDLE_INVALID;
    }
    const Space& target = *reinterpret_cast<Space*>(space);
    const Space& base = *reinterpret_cast<Space*>(baseSpace);
    if (target.is_action_space || base.is_action_space) {
        // Controllers are not tracked: the wheel is the input device.
        location->locationFlags = 0;
        location->pose = pose_identity();
        return XR_SUCCESS;
    }
    location->pose = pose_multiply(pose_inverse(space_origin_in_world(base)), space_origin_in_world(target));
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swapchains
// ---------------------------------------------------------------------------

// BGRA first: the game takes the first format it likes, and BGRA is what the
// video encoder consumes, so the frame copy needs no channel swizzle.
const int64_t kSwapchainFormats[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_D24_UNORM_S8_UINT,
    DXGI_FORMAT_D16_UNORM,
};

bool is_depth_format(int64_t format) {
    return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           format == DXGI_FORMAT_D16_UNORM;
}

XrResult XRAPI_CALL rt_xrEnumerateSwapchainFormats(XrSession session, uint32_t formatCapacityInput,
                                                  uint32_t* formatCountOutput, int64_t* formats) {
    runtime_log("xrEnumerateSwapchainFormats");
    if (!check_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    return return_array(formatCapacityInput, formatCountOutput, formats, kSwapchainFormats,
                        sizeof(kSwapchainFormats) / sizeof(kSwapchainFormats[0]));
}

void destroy_swapchain_locked(Swapchain& swapchain) {
    for (ID3D11Texture2D*& image : swapchain.images) {
        if (image) {
            image->Release();
            image = nullptr;
        }
    }
    for (ID3D12Resource*& image : swapchain.images12) {
        if (image) {
            image->Release();
            image = nullptr;
        }
    }
    swapchain.alive = false;
}

bool create_d3d11_images(Swapchain& slot, const XrSwapchainCreateInfo* createInfo) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = createInfo->width;
    desc.Height = createInfo->height;
    desc.MipLevels = createInfo->mipCount;
    desc.ArraySize = createInfo->arraySize;
    desc.Format = static_cast<DXGI_FORMAT>(createInfo->format);
    desc.SampleDesc.Count = createInfo->sampleCount;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }
    if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    }
    if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) {
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    if (desc.BindFlags == 0) {
        desc.BindFlags = is_depth_format(createInfo->format) ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    }

    for (uint32_t i = 0; i < kSwapchainImageCount; ++i) {
        const HRESULT hr = g_session.device->CreateTexture2D(&desc, nullptr, &slot.images[i]);
        if (FAILED(hr)) {
            runtime_logf("  CreateTexture2D failed: 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

// Images start in the state the D3D12 binding promises the game: render
// target for colour, depth write for depth. The readback moves colour images
// out of it and back around its copy.
bool create_d3d12_images(Swapchain& slot, const XrSwapchainCreateInfo* createInfo) {
    const bool depth = is_depth_format(createInfo->format) ||
                       (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = createInfo->width;
    desc.Height = createInfo->height;
    desc.DepthOrArraySize = static_cast<UINT16>(createInfo->arraySize);
    desc.MipLevels = static_cast<UINT16>(createInfo->mipCount);
    desc.Format = static_cast<DXGI_FORMAT>(createInfo->format);
    desc.SampleDesc.Count = createInfo->sampleCount;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_STATES initial = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

    for (uint32_t i = 0; i < kSwapchainImageCount; ++i) {
        const HRESULT hr = g_session.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial,
                                                                       nullptr, IID_PPV_ARGS(&slot.images12[i]));
        if (FAILED(hr)) {
            runtime_logf("  CreateCommittedResource failed: 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

XrResult XRAPI_CALL rt_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                        XrSwapchain* swapchain) {
    ScopedLock lock;
    if (!check_session(session) || !createInfo || !swapchain) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrCreateSwapchain %ux%u format=%lld samples=%u faces=%u array=%u mips=%u usage=0x%llx flags=0x%llx",
                 createInfo->width, createInfo->height, static_cast<long long>(createInfo->format),
                 createInfo->sampleCount, createInfo->faceCount, createInfo->arraySize, createInfo->mipCount,
                 static_cast<unsigned long long>(createInfo->usageFlags),
                 static_cast<unsigned long long>(createInfo->createFlags));
    log_next_chain("xrCreateSwapchain", createInfo->next);

    bool supported = false;
    for (int64_t format : kSwapchainFormats) {
        supported = supported || format == createInfo->format;
    }
    if (!supported) {
        runtime_log("  unsupported swapchain format");
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    if (createInfo->faceCount != 1) {
        runtime_log("  cube swapchains unsupported");
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    Swapchain* slot = allocate_slot(g_swapchains);
    if (!slot) {
        return XR_ERROR_LIMIT_REACHED;
    }
    slot->info = *createInfo;
    slot->info.next = nullptr;

    const bool created = g_session.api == GraphicsApi::d3d12 ? create_d3d12_images(*slot, createInfo)
                                                              : create_d3d11_images(*slot, createInfo);
    if (!created) {
        destroy_swapchain_locked(*slot);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    *swapchain = reinterpret_cast<XrSwapchain>(slot);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrDestroySwapchain(XrSwapchain swapchain) {
    ScopedLock lock;
    runtime_log("xrDestroySwapchain");
    if (!is_live_handle(g_swapchains, swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_swapchain_locked(*reinterpret_cast<Swapchain*>(swapchain));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                 uint32_t* imageCountOutput,
                                                 XrSwapchainImageBaseHeader* images) {
    ScopedLock lock;
    runtime_logf("xrEnumerateSwapchainImages capacity=%u", imageCapacityInput);
    if (!is_live_handle(g_swapchains, swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!imageCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *imageCountOutput = kSwapchainImageCount;
    if (imageCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (imageCapacityInput < kSwapchainImageCount || !images) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    const Swapchain& chain = *reinterpret_cast<Swapchain*>(swapchain);
    if (g_session.api == GraphicsApi::d3d12) {
        auto* d3d_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (uint32_t i = 0; i < kSwapchainImageCount; ++i) {
            if (d3d_images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
                runtime_logf("  image struct %u has type %s", i, structure_name(d3d_images[i].type));
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d_images[i].texture = chain.images12[i];
        }
        return XR_SUCCESS;
    }
    auto* d3d_images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < kSwapchainImageCount; ++i) {
        if (d3d_images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
            runtime_logf("  image struct %u has type %s", i, structure_name(d3d_images[i].type));
            return XR_ERROR_VALIDATION_FAILURE;
        }
        d3d_images[i].texture = chain.images[i];
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo,
                                              uint32_t* index) {
    ScopedLock lock;
    (void)acquireInfo;
    if (!is_live_handle(g_swapchains, swapchain) || !index) {
        return XR_ERROR_HANDLE_INVALID;
    }
    Swapchain& chain = *reinterpret_cast<Swapchain*>(swapchain);
    ++g_session.acquire_count;
    if (chain.acquired >= 0) {
        runtime_log("xrAcquireSwapchainImage: previous image not released");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain.acquired = static_cast<int>(chain.next);
    chain.next = (chain.next + 1) % kSwapchainImageCount;
    *index = static_cast<uint32_t>(chain.acquired);
    if (should_log_call(g_session.acquire_count)) {
        runtime_logf("xrAcquireSwapchainImage #%llu -> %u", static_cast<unsigned long long>(g_session.acquire_count),
                     *index);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    ScopedLock lock;
    (void)waitInfo;
    if (!is_live_handle(g_swapchains, swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    // Images are never in flight on our side after xrEndFrame returns (the
    // readback copy is issued synchronously), so there is nothing to wait for.
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    ScopedLock lock;
    (void)releaseInfo;
    if (!is_live_handle(g_swapchains, swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    Swapchain& chain = *reinterpret_cast<Swapchain*>(swapchain);
    if (chain.acquired < 0) {
        runtime_log("xrReleaseSwapchainImage: nothing acquired");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain.last_released = chain.acquired;
    chain.acquired = -1;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

XrResult XRAPI_CALL rt_xrWaitFrame(XrSession session, const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState) {
    (void)frameWaitInfo;
    XrTime target_display;
    {
        ScopedLock lock;
        if (!check_session(session) || !frameState) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!g_session.running) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        ++g_session.wait_count;
        const XrTime now = now_ns();
        target_display = g_session.last_predicted_display + kFramePeriodNs;
        if (target_display < now) {
            // Behind schedule: skip whole periods so the cadence keeps its
            // original phase. Snapping to "now" instead lets every slow frame
            // push the average rate permanently below the display rate.
            const XrTime periods_behind = (now - target_display) / kFramePeriodNs + 1;
            target_display += periods_behind * kFramePeriodNs;
        }
        g_session.last_predicted_display = target_display;
    }

    // Pace to the display rate: return one period before the predicted display
    // time so the app has a full frame to render.
    sleep_until(target_display - kFramePeriodNs);

    frameState->predictedDisplayTime = target_display;
    frameState->predictedDisplayPeriod = kFramePeriodNs;
    frameState->shouldRender = XR_TRUE;
    if (should_log_call(g_session.wait_count)) {
        runtime_logf("xrWaitFrame #%llu display=%lld", static_cast<unsigned long long>(g_session.wait_count),
                     static_cast<long long>(target_display));
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) {
    ScopedLock lock;
    (void)frameBeginInfo;
    if (!check_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!g_session.running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    ++g_session.begin_count;
    const bool discarded = g_session.frame_begun;
    g_session.frame_begun = true;
    if (should_log_call(g_session.begin_count) || discarded) {
        runtime_logf("xrBeginFrame #%llu%s", static_cast<unsigned long long>(g_session.begin_count),
                     discarded ? " (previous frame discarded)" : "");
    }
    return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState,
                                    uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views) {
    ScopedLock lock;
    if (!check_session(session) || !viewLocateInfo || !viewState || !viewCountOutput) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (!is_live_handle(g_spaces, viewLocateInfo->space)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    ++g_session.locate_views_count;
    *viewCountOutput = 2;
    if (viewCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (viewCapacityInput < 2 || !views) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    // The game may ask several times for the same display time (culling, then
    // rendering). Every call must get the same pose, or the frame is rendered
    // with one tracking sample and stamped with another, and the headset's
    // reprojection warps the image as the head moves.
    LocatedFrame* located = nullptr;
    for (LocatedFrame& candidate : g_located) {
        if (candidate.display_time == viewLocateInfo->displayTime) {
            located = &candidate;
        }
    }
    if (located) {
        g_head_pose = located->head;
        g_last_tracking_timestamp_ns = located->tracking_timestamp_ns;
    } else {
        refresh_tracking();
        located = &g_located[g_located_next];
        *located = LocatedFrame{viewLocateInfo->displayTime, g_last_tracking_timestamp_ns, g_head_pose};
        g_located_next = (g_located_next + 1) % (sizeof(g_located) / sizeof(g_located[0]));
    }

    // Head in the requested space, then each eye offset sideways.
    const Space& base = *reinterpret_cast<Space*>(viewLocateInfo->space);
    Space view_space{};
    view_space.alive = true;
    view_space.reference_type = XR_REFERENCE_SPACE_TYPE_VIEW;
    const XrPosef head = pose_multiply(pose_inverse(space_origin_in_world(base)), space_origin_in_world(view_space));

    for (uint32_t eye = 0; eye < 2; ++eye) {
        XrPosef eye_offset = pose_identity();
        eye_offset.position.x = (eye == 0 ? -0.5f : 0.5f) * g_ipd;
        views[eye].pose = pose_multiply(head, eye_offset);
        views[eye].fov = g_fov[eye];
    }
    viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;

    if (should_log_call(g_session.locate_views_count)) {
        runtime_logf("xrLocateViews #%llu space=%s time=%lld head=(%.3f %.3f %.3f %.3f) pos=(%.2f %.2f %.2f) tracking=%s",
                     static_cast<unsigned long long>(g_session.locate_views_count),
                     base.is_action_space ? "action" : reference_space_name(base.reference_type),
                     static_cast<long long>(viewLocateInfo->displayTime), head.orientation.x, head.orientation.y,
                     head.orientation.z, head.orientation.w, head.position.x, head.position.y, head.position.z,
                     g_have_tracking ? "live" : "fixed");
    }
    return XR_SUCCESS;
}

// The most recently released image of `chain` as the readback sees it, or an
// empty source before the game has released one.
EyeSource eye_source(const Swapchain& chain, uint32_t array_index) {
    EyeSource source;
    source.array_index = array_index;
    if (chain.last_released >= 0) {
        source.texture11 = chain.images[chain.last_released];
        source.resource12 = chain.images12[chain.last_released];
    }
    return source;
}

// Reads the released image of `chain` back and writes it as PNG. Only 8-bit RGBA/BGRA formats are converted; anything else
// is reported so we learn what the game actually renders into.
void dump_eye_texture(Swapchain& chain, uint32_t array_index, const XrRect2Di& rect, uint32_t eye, uint64_t frame) {
    if (chain.last_released < 0) {
        runtime_logf("  eye %u: swapchain has no released image", eye);
        return;
    }
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(chain.info.format);
    bool bgra = false;
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        bgra = true;
        break;
    default:
        runtime_logf("  eye %u: format %d not convertible to PNG yet", eye, static_cast<int>(format));
        return;
    }

    const LARGE_INTEGER start = [] { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c; }();
    const std::size_t stride = static_cast<std::size_t>(chain.info.width) * 4;
    auto* pixels_owned = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, stride * chain.info.height, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pixels_owned) {
        return;
    }
    const EyeSource source = eye_source(chain, array_index);
    if (!g_session.readback->snapshot(source, chain.info.width, chain.info.height, pixels_owned)) {
        runtime_logf("  eye %u: readback failed", eye);
        VirtualFree(pixels_owned, 0, MEM_RELEASE);
        return;
    }
    LARGE_INTEGER mapped_at;
    QueryPerformanceCounter(&mapped_at);

    char path[MAX_PATH] = {};
    if (!runtime_module_directory(path, MAX_PATH)) {
        VirtualFree(pixels_owned, 0, MEM_RELEASE);
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "wheelio_xr_frame%llu_eye%u.png", static_cast<unsigned long long>(frame), eye);
    std::strncat(path, name, MAX_PATH - std::strlen(path) - 1);

    // Honour the sub-rectangle the layer actually uses.
    const uint32_t x0 = rect.offset.x < 0 ? 0 : static_cast<uint32_t>(rect.offset.x);
    const uint32_t y0 = rect.offset.y < 0 ? 0 : static_cast<uint32_t>(rect.offset.y);
    uint32_t width = rect.extent.width > 0 ? static_cast<uint32_t>(rect.extent.width) : chain.info.width;
    uint32_t height = rect.extent.height > 0 ? static_cast<uint32_t>(rect.extent.height) : chain.info.height;
    if (x0 + width > chain.info.width) {
        width = chain.info.width - x0;
    }
    if (y0 + height > chain.info.height) {
        height = chain.info.height - y0;
    }
    const std::uint8_t* pixels = pixels_owned + y0 * stride + x0 * 4;
    const bool ok = write_png_rgb(path, pixels, width, height, static_cast<std::uint32_t>(stride), bgra);
    VirtualFree(pixels_owned, 0, MEM_RELEASE);

    const double to_ms = 1000.0 / static_cast<double>(g_qpc_frequency.QuadPart);
    runtime_logf("  eye %u: %s %ux%u (readback %.2f ms, png %.1f ms) -> %s", eye, ok ? "wrote" : "FAILED", width,
                 height, static_cast<double>(mapped_at.QuadPart - start.QuadPart) * to_ms,
                 static_cast<double>(([] { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c; }().QuadPart) -
                                     mapped_at.QuadPart) *
                     to_ms,
                 path);
}

XrResult XRAPI_CALL rt_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    const uint64_t entered_at_ns = static_cast<uint64_t>(now_ns());
    FILETIME entered_file_time;
    GetSystemTimePreciseAsFileTime(&entered_file_time);
    const uint64_t entered_at_unix_ns =
        (((static_cast<uint64_t>(entered_file_time.dwHighDateTime) << 32) | entered_file_time.dwLowDateTime) -
         116444736000000000ULL) * 100ULL;
    ScopedLock lock;
    if (!check_session(session) || !frameEndInfo) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!g_session.running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (!g_session.frame_begun) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    g_session.frame_begun = false;
    ++g_session.end_count;
    const uint64_t frame = g_session.end_count;

    bool dump = false;
    if (g_dump_frames_enabled) {
        for (uint64_t dump_frame : kDumpFrames) {
            dump = dump || dump_frame == frame;
        }
    }
    const bool verbose = should_log_call(frame) || dump;
    if (verbose) {
        runtime_logf("xrEndFrame #%llu display=%lld blend=%d layers=%u", static_cast<unsigned long long>(frame),
                     static_cast<long long>(frameEndInfo->displayTime), static_cast<int>(frameEndInfo->environmentBlendMode),
                     frameEndInfo->layerCount);
        log_next_chain("xrEndFrame", frameEndInfo->next);
    }

    // Filled in from the projection layer, then handed to the exporter below.
    FrameExporter::ViewInfo exported_views[2];
    bool exportable = false;

    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[i];
        if (!layer) {
            return XR_ERROR_LAYER_INVALID;
        }
        if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            if (verbose) {
                runtime_logf("  layer %u: %s (ignored)", i, structure_name(layer->type));
            }
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (verbose) {
            runtime_logf("  layer %u: projection views=%u flags=0x%llx", i, projection->viewCount,
                         static_cast<unsigned long long>(projection->layerFlags));
            log_next_chain("  projection layer", projection->next);
        }
        if (!is_live_handle(g_spaces, projection->space)) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (projection->viewCount < 2) {
            continue;
        }

        exportable = true;
        for (uint32_t v = 0; v < projection->viewCount && v < 2; ++v) {
            const XrCompositionLayerProjectionView& view = projection->views[v];
            if (!is_live_handle(g_swapchains, view.subImage.swapchain)) {
                return XR_ERROR_HANDLE_INVALID;
            }
            Swapchain& chain = *reinterpret_cast<Swapchain*>(view.subImage.swapchain);
            if (verbose) {
                runtime_logf("    view %u: swapchain %ux%u rect=(%d,%d %dx%d) array=%u pos=(%.3f %.3f %.3f) "
                             "fov=(%.3f %.3f %.3f %.3f)",
                             v, chain.info.width, chain.info.height, view.subImage.imageRect.offset.x,
                             view.subImage.imageRect.offset.y, view.subImage.imageRect.extent.width,
                             view.subImage.imageRect.extent.height, view.subImage.imageArrayIndex,
                             view.pose.position.x, view.pose.position.y, view.pose.position.z, view.fov.angleLeft,
                             view.fov.angleRight, view.fov.angleUp, view.fov.angleDown);
                log_next_chain("    projection view", view.next);
            }
            if (dump) {
                dump_eye_texture(chain, view.subImage.imageArrayIndex, view.subImage.imageRect, v, frame);
            }

            FrameExporter::ViewInfo& exported = exported_views[v];
            exported.source = eye_source(chain, view.subImage.imageArrayIndex);
            exported.orientation[0] = view.pose.orientation.x;
            exported.orientation[1] = view.pose.orientation.y;
            exported.orientation[2] = view.pose.orientation.z;
            exported.orientation[3] = view.pose.orientation.w;
            exported.position[0] = view.pose.position.x;
            exported.position[1] = view.pose.position.y;
            exported.position[2] = view.pose.position.z;
            exported.fov[0] = view.fov.angleLeft;
            exported.fov[1] = view.fov.angleRight;
            exported.fov[2] = view.fov.angleUp;
            exported.fov[3] = view.fov.angleDown;
            exported.tracking_timestamp_ns = g_last_tracking_timestamp_ns;
            bool matched = false;
            for (const LocatedFrame& located : g_located) {
                if (located.display_time == frameEndInfo->displayTime) {
                    exported.tracking_timestamp_ns = located.tracking_timestamp_ns;
                    matched = true;
                }
            }
            if (!matched && v == 0) {
                ++g_unmatched_frames;
                if (should_log_call(g_unmatched_frames)) {
                    runtime_logf("xrEndFrame: display time %lld was never located (%llu so far)",
                                 static_cast<long long>(frameEndInfo->displayTime),
                                 static_cast<unsigned long long>(g_unmatched_frames));
                }
            }

            // Every eye must come from a swapchain of the size the exporter was
            // started with, or the copy would not match the shared layout.
            exportable = exportable && exported.source.valid() && chain.info.width == g_view_width &&
                         chain.info.height == g_view_height;
        }

        if (exportable) {
            const Swapchain& first =
                *reinterpret_cast<Swapchain*>(projection->views[0].subImage.swapchain);
            if (!g_frame_exporter.started()) {
                g_frame_exporter.start(g_session.readback, first.info.width, first.info.height,
                                       static_cast<DXGI_FORMAT>(first.info.format));
            }
            g_frame_exporter.note_frame_submitted();
            if (g_frame_exporter.started() && g_frame_exporter.reader_present(entered_at_ns)) {
                g_frame_exporter.submit_frame(static_cast<uint32_t>(frame),
                                              static_cast<uint64_t>(frameEndInfo->displayTime),
                                              entered_at_unix_ns, exported_views);
            }
        }
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Input: accepted, never active. The wheel is handled by the dinput8 proxy.
// ---------------------------------------------------------------------------

XrResult XRAPI_CALL rt_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo,
                                        XrActionSet* actionSet) {
    ScopedLock lock;
    if (!check_instance(instance) || !createInfo || !actionSet) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrCreateActionSet \"%s\" (%s) priority=%u", createInfo->actionSetName,
                 createInfo->localizedActionSetName, createInfo->priority);
    OpaqueHandle* slot = allocate_slot(g_action_sets);
    if (!slot) {
        return XR_ERROR_LIMIT_REACHED;
    }
    copy_string(slot->name, sizeof(slot->name), createInfo->actionSetName);
    *actionSet = reinterpret_cast<XrActionSet>(slot);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrDestroyActionSet(XrActionSet actionSet) {
    ScopedLock lock;
    runtime_log("xrDestroyActionSet");
    if (!is_live_handle(g_action_sets, actionSet)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    reinterpret_cast<OpaqueHandle*>(actionSet)->alive = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action) {
    ScopedLock lock;
    if (!is_live_handle(g_action_sets, actionSet) || !createInfo || !action) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrCreateAction \"%s\" type=%d subactions=%u", createInfo->actionName,
                 static_cast<int>(createInfo->actionType), createInfo->countSubactionPaths);
    OpaqueHandle* slot = allocate_slot(g_actions);
    if (!slot) {
        return XR_ERROR_LIMIT_REACHED;
    }
    copy_string(slot->name, sizeof(slot->name), createInfo->actionName);
    *action = reinterpret_cast<XrAction>(slot);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrDestroyAction(XrAction action) {
    ScopedLock lock;
    runtime_log("xrDestroyAction");
    if (!is_live_handle(g_actions, action)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    reinterpret_cast<OpaqueHandle*>(action)->alive = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) {
    ScopedLock lock;
    if (!check_instance(instance) || !suggestedBindings) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrSuggestInteractionProfileBindings profile=%s bindings=%u",
                 path_string(suggestedBindings->interactionProfile), suggestedBindings->countSuggestedBindings);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo) {
    ScopedLock lock;
    if (!check_session(session) || !attachInfo) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrAttachSessionActionSets count=%u", attachInfo->countActionSets);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                     XrInteractionProfileState* interactionProfile) {
    ScopedLock lock;
    if (!check_session(session) || !interactionProfile) {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime_logf("xrGetCurrentInteractionProfile user=%s", path_string(topLevelUserPath));
    interactionProfile->interactionProfile = XR_NULL_PATH;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                              XrActionStateBoolean* state) {
    if (!check_session(session) || !getInfo || !state) {
        return XR_ERROR_HANDLE_INVALID;
    }
    state->currentState = XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                            XrActionStateFloat* state) {
    if (!check_session(session) || !getInfo || !state) {
        return XR_ERROR_HANDLE_INVALID;
    }
    state->currentState = 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                               XrActionStateVector2f* state) {
    if (!check_session(session) || !getInfo || !state) {
        return XR_ERROR_HANDLE_INVALID;
    }
    state->currentState = XrVector2f{0.0f, 0.0f};
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo,
                                           XrActionStatePose* state) {
    if (!check_session(session) || !getInfo || !state) {
        return XR_ERROR_HANDLE_INVALID;
    }
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
    ScopedLock lock;
    if (!check_session(session) || !syncInfo) {
        return XR_ERROR_HANDLE_INVALID;
    }
    ++g_session.sync_actions_count;
    if (should_log_call(g_session.sync_actions_count)) {
        runtime_logf("xrSyncActions #%llu activeSets=%u", static_cast<unsigned long long>(g_session.sync_actions_count),
                     syncInfo->countActiveActionSets);
    }
    return g_session.state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XrResult XRAPI_CALL rt_xrEnumerateBoundSourcesForAction(XrSession session,
                                                       const XrBoundSourcesForActionEnumerateInfo* enumerateInfo,
                                                       uint32_t sourceCapacityInput, uint32_t* sourceCountOutput,
                                                       XrPath* sources) {
    (void)enumerateInfo;
    (void)sourceCapacityInput;
    (void)sources;
    runtime_log("xrEnumerateBoundSourcesForAction");
    if (!check_session(session) || !sourceCountOutput) {
        return XR_ERROR_HANDLE_INVALID;
    }
    *sourceCountOutput = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrGetInputSourceLocalizedName(XrSession session,
                                                    const XrInputSourceLocalizedNameGetInfo* getInfo,
                                                    uint32_t bufferCapacityInput, uint32_t* bufferCountOutput,
                                                    char* buffer) {
    (void)getInfo;
    runtime_log("xrGetInputSourceLocalizedName");
    if (!check_session(session) || !bufferCountOutput) {
        return XR_ERROR_HANDLE_INVALID;
    }
    *bufferCountOutput = 1;
    if (bufferCapacityInput >= 1 && buffer) {
        buffer[0] = '\0';
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL rt_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                            const XrHapticBaseHeader* hapticFeedback) {
    (void)hapticActionInfo;
    (void)hapticFeedback;
    return check_session(session) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL rt_xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) {
    (void)hapticActionInfo;
    return check_session(session) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

struct Entry {
    const char* name;
    PFN_xrVoidFunction function;
    bool needs_instance;
};

#define RT_ENTRY(name, needs_instance) {#name, reinterpret_cast<PFN_xrVoidFunction>(rt_##name), needs_instance}

const Entry kEntries[] = {
    RT_ENTRY(xrEnumerateInstanceExtensionProperties, false),
    RT_ENTRY(xrEnumerateApiLayerProperties, false),
    RT_ENTRY(xrCreateInstance, false),
    RT_ENTRY(xrDestroyInstance, true),
    RT_ENTRY(xrGetInstanceProperties, true),
    RT_ENTRY(xrPollEvent, true),
    RT_ENTRY(xrResultToString, true),
    RT_ENTRY(xrStructureTypeToString, true),
    RT_ENTRY(xrStringToPath, true),
    RT_ENTRY(xrPathToString, true),
    RT_ENTRY(xrGetSystem, true),
    RT_ENTRY(xrGetSystemProperties, true),
    RT_ENTRY(xrEnumerateEnvironmentBlendModes, true),
    RT_ENTRY(xrEnumerateViewConfigurations, true),
    RT_ENTRY(xrGetViewConfigurationProperties, true),
    RT_ENTRY(xrEnumerateViewConfigurationViews, true),
    RT_ENTRY(xrGetD3D11GraphicsRequirementsKHR, true),
    RT_ENTRY(xrGetD3D12GraphicsRequirementsKHR, true),
    RT_ENTRY(xrCreateSession, true),
    RT_ENTRY(xrDestroySession, true),
    RT_ENTRY(xrBeginSession, true),
    RT_ENTRY(xrEndSession, true),
    RT_ENTRY(xrRequestExitSession, true),
    RT_ENTRY(xrEnumerateReferenceSpaces, true),
    RT_ENTRY(xrCreateReferenceSpace, true),
    RT_ENTRY(xrGetReferenceSpaceBoundsRect, true),
    RT_ENTRY(xrCreateActionSpace, true),
    RT_ENTRY(xrDestroySpace, true),
    RT_ENTRY(xrLocateSpace, true),
    RT_ENTRY(xrEnumerateSwapchainFormats, true),
    RT_ENTRY(xrCreateSwapchain, true),
    RT_ENTRY(xrDestroySwapchain, true),
    RT_ENTRY(xrEnumerateSwapchainImages, true),
    RT_ENTRY(xrAcquireSwapchainImage, true),
    RT_ENTRY(xrWaitSwapchainImage, true),
    RT_ENTRY(xrReleaseSwapchainImage, true),
    RT_ENTRY(xrWaitFrame, true),
    RT_ENTRY(xrBeginFrame, true),
    RT_ENTRY(xrEndFrame, true),
    RT_ENTRY(xrLocateViews, true),
    RT_ENTRY(xrCreateActionSet, true),
    RT_ENTRY(xrDestroyActionSet, true),
    RT_ENTRY(xrCreateAction, true),
    RT_ENTRY(xrDestroyAction, true),
    RT_ENTRY(xrSuggestInteractionProfileBindings, true),
    RT_ENTRY(xrAttachSessionActionSets, true),
    RT_ENTRY(xrGetCurrentInteractionProfile, true),
    RT_ENTRY(xrGetActionStateBoolean, true),
    RT_ENTRY(xrGetActionStateFloat, true),
    RT_ENTRY(xrGetActionStateVector2f, true),
    RT_ENTRY(xrGetActionStatePose, true),
    RT_ENTRY(xrSyncActions, true),
    RT_ENTRY(xrEnumerateBoundSourcesForAction, true),
    RT_ENTRY(xrGetInputSourceLocalizedName, true),
    RT_ENTRY(xrApplyHapticFeedback, true),
    RT_ENTRY(xrStopHapticFeedback, true),
};

#undef RT_ENTRY

XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!function) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;
    if (!name) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(rt_xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            if (entry.needs_instance && !check_instance(instance)) {
                runtime_logf("xrGetInstanceProcAddr(%s) without a valid instance", name);
                return XR_ERROR_HANDLE_INVALID;
            }
            *function = entry.function;
            return XR_SUCCESS;
        }
    }
    // Loud on purpose: this is how we learn what the game needs.
    runtime_logf("xrGetInstanceProcAddr(%s) -> UNSUPPORTED", name);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// Names for logs
// ---------------------------------------------------------------------------

const char* result_name(XrResult result) {
    switch (result) {
#define RT_CASE(value, number) case value: return #value;
        XR_LIST_ENUM_XrResult(RT_CASE)
#undef RT_CASE
    default:
        return "XR_UNKNOWN_RESULT";
    }
}

const char* structure_name(XrStructureType type) {
    switch (type) {
#define RT_CASE(value, number) case value: return #value;
        XR_LIST_ENUM_XrStructureType(RT_CASE)
#undef RT_CASE
    default:
        return "XR_UNKNOWN_STRUCTURE_TYPE";
    }
}

const char* session_state_name(XrSessionState state) {
    switch (state) {
#define RT_CASE(value, number) case value: return #value;
        XR_LIST_ENUM_XrSessionState(RT_CASE)
#undef RT_CASE
    default:
        return "XR_SESSION_STATE_?";
    }
}

const char* reference_space_name(XrReferenceSpaceType type) {
    switch (type) {
#define RT_CASE(value, number) case value: return #value;
        XR_LIST_ENUM_XrReferenceSpaceType(RT_CASE)
#undef RT_CASE
    default:
        return "XR_REFERENCE_SPACE_TYPE_?";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest) {
    if (!loaderInfo || !runtimeRequest || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest)) {
        runtime_log("xrNegotiateLoaderRuntimeInterface: bad loader structs");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    runtime_logf("xrNegotiateLoaderRuntimeInterface loader interface %u..%u api %u.%u.%u..%u.%u.%u",
                 loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion,
                 XR_VERSION_MAJOR(loaderInfo->minApiVersion), XR_VERSION_MINOR(loaderInfo->minApiVersion),
                 XR_VERSION_PATCH(loaderInfo->minApiVersion), XR_VERSION_MAJOR(loaderInfo->maxApiVersion),
                 XR_VERSION_MINOR(loaderInfo->maxApiVersion), XR_VERSION_PATCH(loaderInfo->maxApiVersion));

    if (XR_CURRENT_LOADER_RUNTIME_VERSION < loaderInfo->minInterfaceVersion ||
        XR_CURRENT_LOADER_RUNTIME_VERSION > loaderInfo->maxInterfaceVersion) {
        runtime_log("  loader interface version mismatch");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (XR_VERSION_MAJOR(loaderInfo->minApiVersion) > 1 || XR_VERSION_MAJOR(loaderInfo->maxApiVersion) < 1) {
        runtime_log("  api major version mismatch");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
    runtimeRequest->getInstanceProcAddr = rt_xrGetInstanceProcAddr;
    return XR_SUCCESS;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_lock);
        QueryPerformanceFrequency(&g_qpc_frequency);
        runtime_log_set_module(instance);
        runtime_log("runtime loaded");
    }
    return TRUE;
}
