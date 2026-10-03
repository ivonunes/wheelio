#include "bridge_client.hpp"
#include "constants.hpp"
#include "dinput_effects.hpp"
#include "wheelio_bridge_protocol.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dinput.h>
#include <windows.h>

void* operator new(std::size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size));
}

void operator delete(void* pointer) noexcept {
    if (pointer) {
        HeapFree(GetProcessHeap(), 0, pointer);
    }
}

void operator delete(void* pointer, std::size_t) noexcept {
    if (pointer) {
        HeapFree(GetProcessHeap(), 0, pointer);
    }
}

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT(WINAPI*)();
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT(WINAPI*)();
using DllUnregisterServerFn = HRESULT(WINAPI*)();

constexpr int kMaxEffects = 64;
constexpr DWORD kSyntheticDrivingType = DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_THREEPEDALS << 8);
constexpr ULONGLONG kPeriodicUpdateIntervalUs = 4000ULL;
constexpr DWORD kRuntimeUpdateIntervalMs = 4;

HMODULE g_real_dinput8 = nullptr;
HMODULE g_this_module = nullptr;
DirectInput8CreateFn g_real_create = nullptr;
DllCanUnloadNowFn g_real_can_unload = nullptr;
DllGetClassObjectFn g_real_get_class_object = nullptr;
DllRegisterServerFn g_real_register_server = nullptr;
DllUnregisterServerFn g_real_unregister_server = nullptr;
BridgeClient g_bridge_client;
volatile LONG g_bridge_announced = 0;
INIT_ONCE g_real_dinput_init_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_bridge_client_init_once = INIT_ONCE_STATIC_INIT;
bool g_real_dinput_loaded = false;

void append_proxy_logf(const char* format, ...);

// What the synthetic force-feedback axis tells the game it can push, in
// newtons (DIDEVICEOBJECTINSTANCE::dwFFMaxForce is a force, not a level).
// Games that read it divide the force they want by it: reporting
// DI_FFNOMINALMAX (10000) made ETS2 send at most a few thousandths of full
// scale. 0 ("not stated") is what Wine's own DirectInput reports for real
// force-feedback wheels; ETS2 then scales its forces itself and feels the
// same as on Windows.
constexpr DWORD kReportedMaxForceNewtons = 0;

inline DWORD clamp_dword(DWORD value, DWORD minimum, DWORD maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

inline DWORD min_dword(DWORD a, DWORD b) {
    return (a < b) ? a : b;
}

inline bool is_game_controller_type(DWORD dev_type) {
    const DWORD base_type = GET_DIDEVICE_TYPE(dev_type);
    return base_type == DI8DEVTYPE_JOYSTICK || base_type == DI8DEVTYPE_GAMEPAD || base_type == DI8DEVTYPE_DRIVING ||
           base_type == DIDEVTYPE_JOYSTICK;
}

bool is_supported_wheel_vid_pid(DWORD vendor_id, DWORD product_id) {
    const WheelProfile* profile = find_wheel_profile(vendor_id, product_id);
    return profile && profile->force_feedback_supported;
}

bool product_guid_matches_supported_wheel(REFGUID product_guid) {
    const DWORD low_word = LOWORD(product_guid.Data1);
    const DWORD high_word = HIWORD(product_guid.Data1);
    return is_supported_wheel_vid_pid(low_word, high_word) ||
           is_supported_wheel_vid_pid(high_word, low_word);
}

bool should_synthesize_force_feedback(REFGUID product_guid, DWORD dev_type) {
    (void)dev_type;
    return product_guid_matches_supported_wheel(product_guid);
}

void log_product_guid(const char* prefix, REFGUID product_guid, DWORD dev_type, bool synthetic) {
    append_proxy_logf("%s product_guid_data1=0x%08lx lo=0x%04lx hi=0x%04lx devtype=0x%08lx synthetic_ffb=%lu",
                      prefix,
                      static_cast<unsigned long>(product_guid.Data1),
                      static_cast<unsigned long>(LOWORD(product_guid.Data1)),
                      static_cast<unsigned long>(HIWORD(product_guid.Data1)),
                      static_cast<unsigned long>(dev_type),
                      synthetic ? 1UL : 0UL);
}

bool is_guid_equal(REFGUID a, REFGUID b) {
    return InlineIsEqualGUID(a, b) != FALSE;
}

const char* effect_guid_name(REFGUID guid) {
    if (is_guid_equal(guid, GUID_ConstantForce)) {
        return "ConstantForce";
    }
    if (is_guid_equal(guid, GUID_RampForce)) {
        return "RampForce";
    }
    if (is_guid_equal(guid, GUID_Square)) {
        return "Square";
    }
    if (is_guid_equal(guid, GUID_Sine)) {
        return "Sine";
    }
    if (is_guid_equal(guid, GUID_Triangle)) {
        return "Triangle";
    }
    if (is_guid_equal(guid, GUID_SawtoothUp)) {
        return "SawtoothUp";
    }
    if (is_guid_equal(guid, GUID_SawtoothDown)) {
        return "SawtoothDown";
    }
    if (is_guid_equal(guid, GUID_Spring)) {
        return "Spring";
    }
    if (is_guid_equal(guid, GUID_Damper)) {
        return "Damper";
    }
    if (is_guid_equal(guid, GUID_Inertia)) {
        return "Inertia";
    }
    if (is_guid_equal(guid, GUID_Friction)) {
        return "Friction";
    }
    return "Unknown";
}

wheelio_bridge::DirectInputEffectKind directinput_effect_kind(REFGUID guid) {
    if (is_guid_equal(guid, GUID_ConstantForce)) {
        return wheelio_bridge::DirectInputEffectKind::constant;
    }
    if (is_guid_equal(guid, GUID_RampForce)) {
        return wheelio_bridge::DirectInputEffectKind::ramp;
    }
    if (is_guid_equal(guid, GUID_Square)) {
        return wheelio_bridge::DirectInputEffectKind::square;
    }
    if (is_guid_equal(guid, GUID_Sine)) {
        return wheelio_bridge::DirectInputEffectKind::sine;
    }
    if (is_guid_equal(guid, GUID_Triangle)) {
        return wheelio_bridge::DirectInputEffectKind::triangle;
    }
    if (is_guid_equal(guid, GUID_SawtoothUp)) {
        return wheelio_bridge::DirectInputEffectKind::sawtooth_up;
    }
    if (is_guid_equal(guid, GUID_SawtoothDown)) {
        return wheelio_bridge::DirectInputEffectKind::sawtooth_down;
    }
    if (is_guid_equal(guid, GUID_Spring)) {
        return wheelio_bridge::DirectInputEffectKind::spring;
    }
    if (is_guid_equal(guid, GUID_Damper)) {
        return wheelio_bridge::DirectInputEffectKind::damper;
    }
    if (is_guid_equal(guid, GUID_Inertia)) {
        return wheelio_bridge::DirectInputEffectKind::inertia;
    }
    if (is_guid_equal(guid, GUID_Friction)) {
        return wheelio_bridge::DirectInputEffectKind::friction;
    }
    return wheelio_bridge::DirectInputEffectKind::unknown;
}

wheelio_bridge::DirectInputDirectionMode directinput_direction_mode(DWORD flags) {
    if ((flags & DIEFF_CARTESIAN) != 0) {
        return wheelio_bridge::DirectInputDirectionMode::cartesian;
    }
    if ((flags & DIEFF_SPHERICAL) != 0) {
        return wheelio_bridge::DirectInputDirectionMode::spherical;
    }
    return wheelio_bridge::DirectInputDirectionMode::polar;
}

bool is_property_key(REFGUID prop, ULONG_PTR key) {
    return reinterpret_cast<ULONG_PTR>(&prop) == key;
}

const char* diprop_name(REFGUID prop) {
    if (is_property_key(prop, 1)) return "BUFFERSIZE";
    if (is_property_key(prop, 2)) return "AXISMODE";
    if (is_property_key(prop, 3)) return "GRANULARITY";
    if (is_property_key(prop, 4)) return "RANGE";
    if (is_property_key(prop, 5)) return "DEADZONE";
    if (is_property_key(prop, 6)) return "SATURATION";
    if (is_property_key(prop, 7)) return "FFGAIN";
    if (is_property_key(prop, 8)) return "FFLOAD";
    if (is_property_key(prop, 9)) return "AUTOCENTER";
    return "other";
}

void append_proxy_log(const char* message) {
    char module_path[MAX_PATH] = {0};
    if (!g_this_module || GetModuleFileNameA(g_this_module, module_path, MAX_PATH) == 0) {
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
        return;
    }

    char* separator = std::strrchr(module_path, '\\');
    if (!separator) {
        separator = std::strrchr(module_path, '/');
    }
    if (separator) {
        separator[1] = '\0';
    } else {
        module_path[0] = '\0';
    }

    std::strncat(module_path, "wheelio_proxy.log", MAX_PATH - std::strlen(module_path) - 1);

    const DWORD attrs = GetFileAttributesA(module_path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        HANDLE file = CreateFileA(
            module_path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file != INVALID_HANDLE_VALUE) {
            char line[1024] = {0};
            std::snprintf(line, sizeof(line), "%s\r\n", message);
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            CloseHandle(file);
        }
    }

    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

void append_proxy_logf(const char* format, ...) {
    char buffer[1024] = {0};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    append_proxy_log(buffer);
}

const char* directinput_iid_name(REFIID riid) {
    if (InlineIsEqualGUID(riid, IID_IDirectInput8W) != FALSE) {
        return "IDirectInput8W";
    }
    if (InlineIsEqualGUID(riid, IID_IDirectInput8A) != FALSE) {
        return "IDirectInput8A";
    }
    if (InlineIsEqualGUID(riid, IID_IUnknown) != FALSE) {
        return "IUnknown";
    }
    return "other";
}

BOOL CALLBACK initialize_bridge_client_once(PINIT_ONCE, PVOID, PVOID*) {
    g_bridge_client.set_logger(&append_proxy_log);
    g_bridge_client.initialize();
    return TRUE;
}

void ensure_bridge_client_initialized() {
    InitOnceExecuteOnce(&g_bridge_client_init_once, initialize_bridge_client_once, nullptr, nullptr);
}

bool bridge_send_hello(const char* client_name, std::uint32_t process_id) {
    ensure_bridge_client_initialized();
    return g_bridge_client.send_hello(client_name, process_id);
}

bool bridge_send_state(const wheelio_bridge::WheelStatePayload& payload) {
    ensure_bridge_client_initialized();
    return g_bridge_client.send_state(payload);
}

bool bridge_send_stop_all() {
    ensure_bridge_client_initialized();
    return g_bridge_client.send_stop_all();
}

// The name reported to the Mac app — the host game's executable (the proxy runs
// inside the game), e.g. "amtrucks.exe" -> "amtrucks". Falls back to "Game".
const char* host_process_name() {
    static char cached[64] = {0};
    if (cached[0] != '\0') {
        return cached;
    }

    wchar_t path[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        lstrcpynA(cached, "Game", sizeof(cached));
        return cached;
    }

    wchar_t* base = path;
    for (wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base = p + 1;
        }
    }

    if (WideCharToMultiByte(CP_UTF8, 0, base, -1, cached, sizeof(cached), nullptr, nullptr) == 0) {
        lstrcpynA(cached, "Game", sizeof(cached));
        return cached;
    }

    const size_t n = std::strlen(cached);
    if (n > 4) {
        char* ext = cached + n - 4;
        if (ext[0] == '.' &&
            (ext[1] == 'e' || ext[1] == 'E') &&
            (ext[2] == 'x' || ext[2] == 'X') &&
            (ext[3] == 'e' || ext[3] == 'E')) {
            *ext = '\0';
        }
    }

    if (cached[0] == '\0') {
        lstrcpynA(cached, "Game", sizeof(cached));
    }
    return cached;
}

void announce_bridge_connection() {
    if (InterlockedCompareExchange(&g_bridge_announced, 1, 0) != 0) {
        return;
    }

    const bool queued = bridge_send_hello(host_process_name(), GetCurrentProcessId());
    append_proxy_logf("bridge hello identity %s", queued ? "queued" : "failed");
}

bool load_real_dinput8() {
    if (g_real_dinput8) {
        return true;
    }

    wchar_t system_dir[MAX_PATH] = {0};
    if (GetSystemDirectoryW(system_dir, MAX_PATH) == 0) {
        append_proxy_log("GetSystemDirectoryW failed");
        return false;
    }

    wchar_t dll_path[MAX_PATH] = {0};
    lstrcpynW(dll_path, system_dir, MAX_PATH);
    lstrcatW(dll_path, L"\\dinput8.dll");
    g_real_dinput8 = LoadLibraryW(dll_path);
    if (!g_real_dinput8) {
        append_proxy_log("LoadLibraryW(system dinput8.dll) failed");
        return false;
    }

    const FARPROC create_proc = GetProcAddress(g_real_dinput8, "DirectInput8Create");
    const FARPROC can_unload_proc = GetProcAddress(g_real_dinput8, "DllCanUnloadNow");
    const FARPROC get_class_object_proc = GetProcAddress(g_real_dinput8, "DllGetClassObject");
    const FARPROC register_server_proc = GetProcAddress(g_real_dinput8, "DllRegisterServer");
    const FARPROC unregister_server_proc = GetProcAddress(g_real_dinput8, "DllUnregisterServer");

    std::memcpy(&g_real_create, &create_proc, sizeof(g_real_create));
    std::memcpy(&g_real_can_unload, &can_unload_proc, sizeof(g_real_can_unload));
    std::memcpy(&g_real_get_class_object, &get_class_object_proc, sizeof(g_real_get_class_object));
    std::memcpy(&g_real_register_server, &register_server_proc, sizeof(g_real_register_server));
    std::memcpy(&g_real_unregister_server, &unregister_server_proc, sizeof(g_real_unregister_server));

    const bool loaded = g_real_create != nullptr;
    append_proxy_logf("real dinput8 load %s", loaded ? "succeeded" : "failed");
    return loaded;
}

BOOL CALLBACK initialize_real_dinput_once(PINIT_ONCE, PVOID, PVOID*) {
    g_real_dinput_loaded = load_real_dinput8();
    return TRUE;
}

bool ensure_real_dinput_loaded() {
    InitOnceExecuteOnce(&g_real_dinput_init_once, initialize_real_dinput_once, nullptr, nullptr);
    return g_real_dinput_loaded;
}

// Microsecond monotonic clock. Uses QueryPerformanceCounter for sub-millisecond
// resolution, which the periodic-effect phase and the 4 ms rebuild gate
// (kPeriodicUpdateIntervalUs) rely on; GetTickCount64's ~10-16 ms granularity
// made both jittery. Falls back to GetTickCount64 only if QPC is unavailable.
ULONGLONG now_us() {
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
        return static_cast<ULONGLONG>(GetTickCount64()) * 1000ULL;
    }

    // Split seconds and remainder so the * 1'000'000 scaling cannot overflow on
    // a long-running process.
    const ULONGLONG ticks = static_cast<ULONGLONG>(counter.QuadPart);
    const ULONGLONG freq = static_cast<ULONGLONG>(frequency.QuadPart);
    return (ticks / freq) * 1000000ULL + ((ticks % freq) * 1000000ULL) / freq;
}

// The A and W flavours of DirectInput differ only in string types. Everything
// below is written once against these traits and instantiated for both.
struct DirectInputW {
    using Interface = IDirectInput8W;
    using Device = IDirectInputDevice8W;
    using DeviceInstance = DIDEVICEINSTANCEW;
    using ObjectInstance = DIDEVICEOBJECTINSTANCEW;
    using EffectInfo = DIEFFECTINFOW;
    using ActionFormat = DIACTIONFORMATW;
    using ImageInfoHeader = DIDEVICEIMAGEINFOHEADERW;
    using ConfigureDevicesParams = DICONFIGUREDEVICESPARAMSW;
    using String = LPCWSTR;
    using EnumDevicesCallback = LPDIENUMDEVICESCALLBACKW;
    using EnumObjectsCallback = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffectsCallback = LPDIENUMEFFECTSCALLBACKW;
    using EnumDevicesBySemanticsCallback = LPDIENUMDEVICESBYSEMANTICSCBW;
    static constexpr const char* suffix = "W";
    static bool is_directinput_iid(REFIID riid) {
        return is_guid_equal(riid, IID_IDirectInput8W) || is_guid_equal(riid, IID_IDirectInput7W) ||
               is_guid_equal(riid, IID_IDirectInput2W);
    }
    static bool is_device_iid(REFIID riid) {
        return is_guid_equal(riid, IID_IDirectInputDevice8W) || is_guid_equal(riid, IID_IDirectInputDevice7W) ||
               is_guid_equal(riid, IID_IDirectInputDevice2W) || is_guid_equal(riid, IID_IDirectInputDeviceW);
    }
};

struct DirectInputA {
    using Interface = IDirectInput8A;
    using Device = IDirectInputDevice8A;
    using DeviceInstance = DIDEVICEINSTANCEA;
    using ObjectInstance = DIDEVICEOBJECTINSTANCEA;
    using EffectInfo = DIEFFECTINFOA;
    using ActionFormat = DIACTIONFORMATA;
    using ImageInfoHeader = DIDEVICEIMAGEINFOHEADERA;
    using ConfigureDevicesParams = DICONFIGUREDEVICESPARAMSA;
    using String = LPCSTR;
    using EnumDevicesCallback = LPDIENUMDEVICESCALLBACKA;
    using EnumObjectsCallback = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffectsCallback = LPDIENUMEFFECTSCALLBACKA;
    using EnumDevicesBySemanticsCallback = LPDIENUMDEVICESBYSEMANTICSCBA;
    static constexpr const char* suffix = "A";
    static bool is_directinput_iid(REFIID riid) {
        return is_guid_equal(riid, IID_IDirectInput8A) || is_guid_equal(riid, IID_IDirectInput7A) ||
               is_guid_equal(riid, IID_IDirectInput2A);
    }
    static bool is_device_iid(REFIID riid) {
        return is_guid_equal(riid, IID_IDirectInputDevice8A) || is_guid_equal(riid, IID_IDirectInputDevice7A) ||
               is_guid_equal(riid, IID_IDirectInputDevice2A) || is_guid_equal(riid, IID_IDirectInputDeviceA);
    }
};

template <typename T>
struct EnumObjectContext {
    typename T::EnumObjectsCallback callback;
    LPVOID ref;
    DWORD requested_flags;
    bool actuator_only;
    bool actuator_emitted;
};

template <typename T>
struct EnumDeviceContext {
    typename T::EnumDevicesCallback callback;
    LPVOID ref;
};

struct SupportedEffectDefinition {
    const GUID* guid;
    DWORD type_flags;
    DWORD static_params;
    DWORD dynamic_params;
    const wchar_t* name;
    const char* name_a;
};

template <typename T>
BOOL CALLBACK enum_objects_wrapper(const typename T::ObjectInstance* instance, LPVOID ref) {
    auto* context = static_cast<EnumObjectContext<T>*>(ref);
    if (!context || !context->callback || !instance) {
        return DIENUM_STOP;
    }

    typename T::ObjectInstance patched = *instance;
    const bool is_axis = (instance->dwType & DIDFT_AXIS) != 0;

    if (is_axis && !context->actuator_emitted) {
        patched.dwFlags |= DIDOI_FFACTUATOR;
        patched.dwFFMaxForce = kReportedMaxForceNewtons;
        patched.dwFFForceResolution = 1024;
        context->actuator_emitted = true;
        append_proxy_logf("EnumObjects actuator axis type=0x%08lx max_force=%lu",
                          static_cast<unsigned long>(patched.dwType), static_cast<unsigned long>(patched.dwFFMaxForce));
    }

    if (context->actuator_only && (patched.dwFlags & DIDOI_FFACTUATOR) == 0) {
        return DIENUM_CONTINUE;
    }

    return context->callback(&patched, context->ref);
}

template <typename T>
BOOL CALLBACK enum_devices_wrapper(const typename T::DeviceInstance* instance, LPVOID ref) {
    auto* context = static_cast<EnumDeviceContext<T>*>(ref);
    if (!context || !context->callback || !instance) {
        return DIENUM_STOP;
    }

    typename T::DeviceInstance patched = *instance;
    if (patched.dwSize >= sizeof(typename T::DeviceInstance)) {
        const bool synthetic = should_synthesize_force_feedback(patched.guidProduct, patched.dwDevType);
        log_product_guid("EnumDevices", patched.guidProduct, patched.dwDevType, synthetic);
        if (synthetic) {
            patched.guidFFDriver = CLSID_DirectInputDevice8;
            patched.dwDevType = kSyntheticDrivingType;
            append_proxy_log("EnumDevices injected guidFFDriver");
        }
    }

    return context->callback(&patched, context->ref);
}

const SupportedEffectDefinition* supported_effects() {
    static const SupportedEffectDefinition kEffects[] = {
        {&GUID_ConstantForce, DIEFT_CONSTANTFORCE | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION, L"Constant Force", "Constant Force"},
        {&GUID_RampForce, DIEFT_RAMPFORCE | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION, L"Ramp Force", "Ramp Force"},
        {&GUID_Square, DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE, L"Square", "Square"},
        {&GUID_Sine, DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE, L"Sine", "Sine"},
        {&GUID_Triangle, DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE, L"Triangle", "Triangle"},
        {&GUID_SawtoothUp, DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE, L"Sawtooth Up", "Sawtooth Up"},
        {&GUID_SawtoothDown, DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE, L"Sawtooth Down", "Sawtooth Down"},
        {&GUID_Spring, DIEFT_CONDITION | DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION | DIEFT_DEADBAND,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN, L"Spring", "Spring"},
        {&GUID_Damper, DIEFT_CONDITION | DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN, L"Damper", "Damper"},
        {&GUID_Inertia, DIEFT_CONDITION | DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN, L"Inertia", "Inertia"},
        {&GUID_Friction, DIEFT_CONDITION | DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION,
         DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES,
         DIEP_START | DIEP_TYPESPECIFICPARAMS | DIEP_GAIN, L"Friction", "Friction"},
    };
    return kEffects;
}

constexpr int supported_effect_count() {
    return 11;
}

bool effect_matches_type(const SupportedEffectDefinition& effect, DWORD requested_type) {
    if (requested_type == 0 || requested_type == DIEFT_ALL) {
        return true;
    }

    const DWORD requested_base_type = DIEFT_GETTYPE(requested_type);
    if (requested_base_type != 0 && DIEFT_GETTYPE(effect.type_flags) != requested_base_type) {
        return false;
    }

    const DWORD requested_flags = requested_type & ~0xFFu;
    return requested_flags == 0 || (effect.type_flags & requested_flags) == requested_flags;
}

HRESULT populate_effect_info(LPDIEFFECTINFOW info, const SupportedEffectDefinition& effect) {
    if (!info || info->dwSize < sizeof(DIEFFECTINFOW)) {
        return DIERR_INVALIDPARAM;
    }

    std::memset(info, 0, sizeof(DIEFFECTINFOW));
    info->dwSize = sizeof(DIEFFECTINFOW);
    info->guid = *effect.guid;
    info->dwEffType = effect.type_flags;
    info->dwStaticParams = effect.static_params;
    info->dwDynamicParams = effect.dynamic_params;
    lstrcpynW(info->tszName, effect.name, MAX_PATH);
    return DI_OK;
}

HRESULT populate_effect_info(LPDIEFFECTINFOA info, const SupportedEffectDefinition& effect) {
    if (!info || info->dwSize < sizeof(DIEFFECTINFOA)) {
        return DIERR_INVALIDPARAM;
    }

    std::memset(info, 0, sizeof(DIEFFECTINFOA));
    info->dwSize = sizeof(DIEFFECTINFOA);
    info->guid = *effect.guid;
    info->dwEffType = effect.type_flags;
    info->dwStaticParams = effect.static_params;
    info->dwDynamicParams = effect.dynamic_params;
    lstrcpynA(info->tszName, effect.name_a, MAX_PATH);
    return DI_OK;
}

class EffectProxy;

class EffectOwner {
public:
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual void remove_effect(EffectProxy* effect) = 0;
    virtual void stop_effects_except(EffectProxy* keep) = 0;
    virtual void rebuild_and_send() = 0;
    virtual bool has_active_time_varying_effect() const = 0;

protected:
    ~EffectOwner() = default;
};

class EffectProxy final : public IDirectInputEffect {
public:
    EffectProxy(IDirectInputEffect* inner, REFGUID guid, EffectOwner* owner);
    ~EffectProxy() = default;

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT effect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT effect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD flags) override;
    HRESULT STDMETHODCALLTYPE Download() override;
    HRESULT STDMETHODCALLTYPE Unload() override;
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override;

    bool started() const noexcept { return state_.started; }
    bool has_time_varying_force() const;
    bool needs_runtime_tick(ULONGLONG now) const;
    bool service_runtime_tick(ULONGLONG now);
    void force_stop_runtime();
    void shift_runtime_time(ULONGLONG delta_us);
    wheelio_bridge::DirectInputEffectView effect_view() noexcept { return {kind_, &state_}; }
    bool handle_trigger_event(DWORD object_offset, DWORD data);

private:
    void update_from_effect(LPCDIEFFECT effect, DWORD flags);
    void commit_staged(bool preserve_runtime);
    void start_runtime(DWORD iterations);

    volatile LONG ref_count_;
    IDirectInputEffect* inner_;
    EffectOwner* owner_;
    GUID guid_;
    wheelio_bridge::DirectInputEffectKind kind_;
    wheelio_bridge::DirectInputEffectState state_;
    wheelio_bridge::DirectInputEffectState staged_state_;
    bool downloaded_;
    bool dirty_since_download_;
    bool trigger_enabled_;
    DWORD trigger_button_;
    DWORD trigger_repeat_interval_;
    bool trigger_pressed_;
    ULONGLONG last_trigger_start_us_;
};

template <typename T>
class DeviceProxyT final : public T::Device, public EffectOwner {
public:
    DeviceProxyT(typename T::Device* inner, bool synthetic_force_feedback);
    ~DeviceProxyT();

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override;
    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjectsCallback callback, LPVOID ref, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER header) override;
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER header) override;
    HRESULT STDMETHODCALLTYPE Acquire() override;
    HRESULT STDMETHODCALLTYPE Unacquire() override;
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override;
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override;
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE handle) override;
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::ObjectInstance* instance, DWORD object, DWORD how) override;
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DeviceInstance* instance) override;
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override;
    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffectsCallback callback, LPVOID ref, DWORD type) override;
    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffectInfo* info, REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD out) override;
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override;
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override;
    HRESULT STDMETHODCALLTYPE Poll() override;
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::String file, LPDIENUMEFFECTSINFILECALLBACK callback, LPVOID ref, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::String file, DWORD entries, LPDIFILEEFFECT effects, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActionFormat* format, typename T::String user, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActionFormat* format, typename T::String user, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImageInfoHeader* header) override;

    void remove_effect(EffectProxy* effect) override;
    void stop_effects_except(EffectProxy* keep) override;
    void rebuild_and_send() override;
    bool has_active_time_varying_effect() const override;

private:
    static DWORD WINAPI runtime_thread_entry(LPVOID parameter);
    void runtime_loop();
    bool has_runtime_tick_effect(ULONGLONG now) const;

    volatile LONG ref_count_;
    typename T::Device* inner_;
    // Guards every access to the mutable FFB state below (effect table, gain,
    // autocenter, ff_state, payload cache). CRITICAL_SECTION is re-entrant, so
    // locked methods may safely call other locked methods on the same thread.
    // Mutable so const accessors (has_active_time_varying_effect) can lock too.
    mutable CRITICAL_SECTION state_lock_;
    EffectProxy* effects_[kMaxEffects];
    int effect_count_;
    DWORD ff_gain_;
    DWORD autocenter_mode_;
    DWORD ff_state_;
    bool synthetic_force_feedback_;
    bool advertises_force_feedback_;
    bool last_sent_has_state_;
    bool have_last_payload_;
    ULONGLONG last_periodic_rebuild_us_;
    ULONGLONG pause_started_us_;
    wheelio_bridge::WheelStatePayload last_payload_;
    HANDLE runtime_stop_event_;
    HANDLE runtime_thread_;
};

template <typename T>
class DirectInputProxyT final : public T::Interface {
public:
    explicit DirectInputProxyT(typename T::Interface* inner) : ref_count_(1), inner_(inner) {}
    ~DirectInputProxyT() = default;

    ULONG STDMETHODCALLTYPE AddRef() override {
        inner_->AddRef();
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        inner_->Release();
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override {
        if (!out) {
            return E_POINTER;
        }

        if (is_guid_equal(riid, IID_IUnknown) || T::is_directinput_iid(riid)) {
            append_proxy_logf("DirectInputProxy%s::QueryInterface -> %s", T::suffix, directinput_iid_name(riid));
            *out = static_cast<typename T::Interface*>(this);
            AddRef();
            return DI_OK;
        }

        return inner_->QueryInterface(riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, typename T::Device** out, LPUNKNOWN outer) override {
        if (!out) {
            return E_POINTER;
        }

        append_proxy_logf("DirectInputProxy%s::CreateDevice called", T::suffix);
        typename T::Device* device = nullptr;
        const HRESULT result = inner_->CreateDevice(guid, &device, outer);
        if (FAILED(result) || !device) {
            append_proxy_logf("CreateDevice failed: 0x%08lx", static_cast<unsigned long>(result));
            return result;
        }

        typename T::DeviceInstance instance{};
        instance.dwSize = sizeof(typename T::DeviceInstance);
        const HRESULT info_result = device->GetDeviceInfo(&instance);
        const bool synthetic =
            SUCCEEDED(info_result) && should_synthesize_force_feedback(instance.guidProduct, instance.dwDevType);
        if (SUCCEEDED(info_result)) {
            log_product_guid("CreateDevice", instance.guidProduct, instance.dwDevType, synthetic);
        } else {
            append_proxy_logf("CreateDevice GetDeviceInfo failed: 0x%08lx", static_cast<unsigned long>(info_result));
        }

        if (!synthetic) {
            *out = device;
            append_proxy_log("CreateDevice returning raw non-wheel device");
            return result;
        }

        *out = new DeviceProxyT<T>(device, true);
        append_proxy_log("CreateDevice returning wrapped wheel device");
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, typename T::EnumDevicesCallback callback, LPVOID ref, DWORD flags) override {
        if (!callback) {
            return DIERR_INVALIDPARAM;
        }

        append_proxy_logf("DirectInputProxy%s::EnumDevices type=0x%08lx flags=0x%08lx", T::suffix,
                          static_cast<unsigned long>(type),
                          static_cast<unsigned long>(flags));

        EnumDeviceContext<T> context{};
        context.callback = callback;
        context.ref = ref;
        return inner_->EnumDevices(type, enum_devices_wrapper<T>, &context, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override { return inner_->GetDeviceStatus(guid); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override { return inner_->RunControlPanel(window, flags); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override { return inner_->Initialize(instance, version); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guid, typename T::String name, LPGUID out) override { return inner_->FindDevice(guid, name, out); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(
        typename T::String user, typename T::ActionFormat* format, typename T::EnumDevicesBySemanticsCallback callback, LPVOID ref, DWORD flags) override {
        return inner_->EnumDevicesBySemantics(user, format, callback, ref, flags);
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(
        LPDICONFIGUREDEVICESCALLBACK callback, typename T::ConfigureDevicesParams* params, DWORD flags, LPVOID ref) override {
        return inner_->ConfigureDevices(callback, params, flags, ref);
    }

private:
    volatile LONG ref_count_;
    typename T::Interface* inner_;
};

EffectProxy::EffectProxy(IDirectInputEffect* inner, REFGUID guid, EffectOwner* owner)
    : ref_count_(1), inner_(inner), owner_(owner), guid_(guid), kind_(directinput_effect_kind(guid)),
      state_{}, staged_state_{}, downloaded_(false), dirty_since_download_(true),
      trigger_enabled_(false), trigger_button_(DIEB_NOTRIGGER), trigger_repeat_interval_(0),
      trigger_pressed_(false), last_trigger_start_us_(0) {
    if (owner_) {
        owner_->AddRef();
    }
}

ULONG STDMETHODCALLTYPE EffectProxy::AddRef() {
    if (inner_) {
        inner_->AddRef();
    }
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}

ULONG STDMETHODCALLTYPE EffectProxy::Release() {
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (inner_) {
        inner_->Release();
    }
    if (remaining == 0) {
        EffectOwner* owner = owner_;
        if (owner) {
            owner->remove_effect(this);
        }
        if (owner) {
            owner->Release();
        }
        delete this;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE EffectProxy::QueryInterface(REFIID riid, LPVOID* out) {
    if (!out) {
        return E_POINTER;
    }

    if (is_guid_equal(riid, IID_IUnknown) || is_guid_equal(riid, IID_IDirectInputEffect)) {
        *out = static_cast<IDirectInputEffect*>(this);
        AddRef();
        return DI_OK;
    }

    if (!inner_) {
        *out = nullptr;
        return E_NOINTERFACE;
    }

    return inner_->QueryInterface(riid, out);
}

HRESULT STDMETHODCALLTYPE EffectProxy::Initialize(HINSTANCE instance, DWORD version, REFGUID guid) {
    return inner_ ? inner_->Initialize(instance, version, guid) : DI_OK;
}

HRESULT STDMETHODCALLTYPE EffectProxy::GetEffectGuid(LPGUID guid) {
    if (!guid) {
        return E_POINTER;
    }
    *guid = guid_;
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE EffectProxy::GetParameters(LPDIEFFECT effect, DWORD flags) {
    return inner_ ? inner_->GetParameters(effect, flags) : DI_OK;
}

HRESULT STDMETHODCALLTYPE EffectProxy::SetParameters(LPCDIEFFECT effect, DWORD flags) {
    const HRESULT result = inner_ ? inner_->SetParameters(effect, flags) : DI_OK;
    if (SUCCEEDED(result) && effect) {
        append_proxy_logf(
            "EffectProxy::SetParameters effect=%s flags=0x%08lx axes=%lu type_bytes=%lu",
            effect_guid_name(guid_),
            static_cast<unsigned long>(flags),
            static_cast<unsigned long>(effect->cAxes),
            static_cast<unsigned long>(effect->cbTypeSpecificParams));
        update_from_effect(effect, flags);
        if ((flags & DIEP_START) != 0) {
            commit_staged(false);
            start_runtime(1);
            owner_->rebuild_and_send();
            return result;
        }

        if ((flags & DIEP_NODOWNLOAD) != 0) {
            dirty_since_download_ = true;
            return result;
        }

        commit_staged(state_.started);
        owner_->rebuild_and_send();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE EffectProxy::Start(DWORD iterations, DWORD flags) {
    const HRESULT result = inner_ ? inner_->Start(iterations, flags) : DI_OK;
    if (SUCCEEDED(result)) {
        append_proxy_logf("EffectProxy::Start effect=%s iterations=%lu flags=0x%08lx",
                          effect_guid_name(guid_),
                          static_cast<unsigned long>(iterations),
                          static_cast<unsigned long>(flags));
        if ((flags & DIES_SOLO) != 0) {
            owner_->stop_effects_except(this);
        }
        if ((flags & DIES_NODOWNLOAD) == 0 && (!downloaded_ || dirty_since_download_)) {
            commit_staged(false);
        }
        if (!downloaded_) {
            return DIERR_INCOMPLETEEFFECT;
        }
        start_runtime(iterations);
        owner_->rebuild_and_send();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE EffectProxy::Stop() {
    const HRESULT result = inner_ ? inner_->Stop() : DI_OK;
    if (SUCCEEDED(result)) {
        append_proxy_logf("EffectProxy::Stop effect=%s", effect_guid_name(guid_));
        state_.started = false;
        state_.iterations = 1;
        trigger_pressed_ = false;
        owner_->rebuild_and_send();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE EffectProxy::GetEffectStatus(LPDWORD flags) {
    if (!flags) {
        return E_POINTER;
    }
    *flags = wheelio_bridge::directinput_effect_is_temporally_active(state_, now_us()) ? DIEGES_PLAYING : 0;
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE EffectProxy::Download() {
    const HRESULT result = inner_ ? inner_->Download() : DI_OK;
    if (SUCCEEDED(result)) {
        commit_staged(state_.started);
        owner_->rebuild_and_send();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE EffectProxy::Unload() {
    const HRESULT result = inner_ ? inner_->Unload() : DI_OK;
    if (SUCCEEDED(result)) {
        state_.started = false;
        state_.iterations = 1;
        downloaded_ = false;
        dirty_since_download_ = true;
        trigger_pressed_ = false;
        owner_->rebuild_and_send();
    }
    return result;
}
HRESULT STDMETHODCALLTYPE EffectProxy::Escape(LPDIEFFESCAPE escape) { return inner_ ? inner_->Escape(escape) : DI_OK; }

bool EffectProxy::has_time_varying_force() const {
    return wheelio_bridge::directinput_effect_has_time_varying_force(kind_, state_);
}

bool EffectProxy::needs_runtime_tick(ULONGLONG now) const {
    if (wheelio_bridge::directinput_effect_needs_runtime_tick(kind_, state_, now)) {
        return true;
    }

    return trigger_pressed_ &&
           downloaded_ &&
           trigger_repeat_interval_ != wheelio_bridge::kDirectInputInfiniteDuration &&
           state_.duration != wheelio_bridge::kDirectInputInfiniteDuration &&
           state_.duration != 0 &&
           last_trigger_start_us_ != 0;
}

bool EffectProxy::service_runtime_tick(ULONGLONG now) {
    const bool was_started = state_.started;
    wheelio_bridge::directinput_refresh_effect_runtime(kind_, state_, now);
    bool changed = was_started != state_.started;

    if (!trigger_pressed_ ||
        !downloaded_ ||
        trigger_repeat_interval_ == wheelio_bridge::kDirectInputInfiniteDuration ||
        state_.duration == wheelio_bridge::kDirectInputInfiniteDuration ||
        state_.duration == 0 ||
        last_trigger_start_us_ == 0) {
        return changed;
    }

    const ULONGLONG restart_time =
        last_trigger_start_us_ +
        static_cast<ULONGLONG>(state_.duration) +
        static_cast<ULONGLONG>(trigger_repeat_interval_);
    if (!state_.started && now >= restart_time) {
        start_runtime(1);
        last_trigger_start_us_ = state_.start_time_us;
        changed = true;
    }

    return changed;
}

void EffectProxy::force_stop_runtime() {
    wheelio_bridge::directinput_force_stop_effect(state_);
    trigger_pressed_ = false;
}

void EffectProxy::shift_runtime_time(ULONGLONG delta_us) {
    if (state_.start_time_us != 0) {
        state_.start_time_us += delta_us;
    }
    if (last_trigger_start_us_ != 0) {
        last_trigger_start_us_ += delta_us;
    }
}

bool EffectProxy::handle_trigger_event(DWORD object_offset, DWORD data) {
    if (!trigger_enabled_ || trigger_button_ != object_offset) {
        return false;
    }

    const bool pressed = (data & 0x80u) != 0;
    if (pressed == trigger_pressed_) {
        return false;
    }

    trigger_pressed_ = pressed;
    if (!pressed) {
        state_.started = false;
        state_.iterations = 1;
        return true;
    }

    if (!downloaded_ || dirty_since_download_) {
        commit_staged(false);
    }
    start_runtime(1);
    last_trigger_start_us_ = state_.start_time_us;
    return true;
}

void EffectProxy::update_from_effect(LPCDIEFFECT effect, DWORD flags) {
    if (!effect) {
        return;
    }

    const bool all_params = (flags & DIEP_ALLPARAMS) == DIEP_ALLPARAMS;
    if (all_params || (flags & DIEP_GAIN) != 0) {
        staged_state_.effect_gain = clamp_dword(effect->dwGain, 0, DI_FFNOMINALMAX);
    }
    if (all_params || (flags & DIEP_DURATION) != 0) {
        staged_state_.duration = effect->dwDuration;
    }
    if (all_params || (flags & DIEP_STARTDELAY) != 0) {
        staged_state_.start_delay = effect->dwStartDelay;
    }
    if ((all_params || (flags & DIEP_AXES) != 0) && effect->cAxes > 0) {
        staged_state_.axis_count = effect->cAxes;
    }
    if (all_params || (flags & DIEP_TRIGGERBUTTON) != 0) {
        trigger_button_ = effect->dwTriggerButton;
        trigger_enabled_ = trigger_button_ != DIEB_NOTRIGGER &&
                           (effect->dwFlags & DIEFF_OBJECTOFFSETS) != 0;
        trigger_pressed_ = false;
    }
    if (all_params || (flags & DIEP_TRIGGERREPEATINTERVAL) != 0) {
        trigger_repeat_interval_ = effect->dwTriggerRepeatInterval;
    }
    if ((all_params || (flags & DIEP_DIRECTION) != 0) && effect->cAxes > 0 && effect->rglDirection) {
        staged_state_.axis_count = effect->cAxes;
        staged_state_.direction_mode = directinput_direction_mode(effect->dwFlags);
        staged_state_.direction[0] = effect->rglDirection[0];
        staged_state_.direction[1] = (effect->cAxes > 1) ? effect->rglDirection[1] : effect->rglDirection[0];
    }
    if (all_params || (flags & DIEP_ENVELOPE) != 0) {
        staged_state_.envelope.enabled = effect->lpEnvelope != nullptr;
        if (effect->lpEnvelope) {
            staged_state_.envelope.attack_level = effect->lpEnvelope->dwAttackLevel;
            staged_state_.envelope.attack_time = effect->lpEnvelope->dwAttackTime;
            staged_state_.envelope.fade_level = effect->lpEnvelope->dwFadeLevel;
            staged_state_.envelope.fade_time = effect->lpEnvelope->dwFadeTime;
        }
    }

    if ((all_params || (flags & DIEP_TYPESPECIFICPARAMS) != 0) &&
        effect->lpvTypeSpecificParams && effect->cbTypeSpecificParams > 0) {
        if (is_guid_equal(guid_, GUID_Spring) || is_guid_equal(guid_, GUID_Damper) ||
            is_guid_equal(guid_, GUID_Friction) || is_guid_equal(guid_, GUID_Inertia)) {
            const auto* conditions = static_cast<const DICONDITION*>(effect->lpvTypeSpecificParams);
            staged_state_.condition_count = min_dword(2, effect->cbTypeSpecificParams / sizeof(DICONDITION));
            for (DWORD i = 0; i < staged_state_.condition_count; ++i) {
                staged_state_.conditions[i].offset = conditions[i].lOffset;
                staged_state_.conditions[i].positive_coefficient = conditions[i].lPositiveCoefficient;
                staged_state_.conditions[i].negative_coefficient = conditions[i].lNegativeCoefficient;
                staged_state_.conditions[i].positive_saturation = conditions[i].dwPositiveSaturation;
                staged_state_.conditions[i].negative_saturation = conditions[i].dwNegativeSaturation;
                staged_state_.conditions[i].deadband = conditions[i].lDeadBand;
            }
            if (staged_state_.condition_count == 1) {
                staged_state_.conditions[1] = staged_state_.conditions[0];
            }
        } else if (is_guid_equal(guid_, GUID_ConstantForce) &&
                   effect->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
            staged_state_.constant_magnitude = static_cast<const DICONSTANTFORCE*>(effect->lpvTypeSpecificParams)->lMagnitude;
        } else if (is_guid_equal(guid_, GUID_RampForce) &&
                   effect->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
            const auto* ramp = static_cast<const DIRAMPFORCE*>(effect->lpvTypeSpecificParams);
            staged_state_.ramp_start = ramp->lStart;
            staged_state_.ramp_end = ramp->lEnd;
        } else if (effect->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
            const auto* periodic = static_cast<const DIPERIODIC*>(effect->lpvTypeSpecificParams);
            staged_state_.periodic_magnitude = periodic->dwMagnitude;
            staged_state_.periodic_offset = periodic->lOffset;
            staged_state_.periodic_phase = periodic->dwPhase;
            staged_state_.periodic_period = periodic->dwPeriod;
            if (staged_state_.periodic_period == 0) {
                staged_state_.periodic_period = wheelio_bridge::kDirectInputDefaultPeriodicPeriodUs;
            }
        }
    }
    dirty_since_download_ = true;

    if (is_guid_equal(guid_, GUID_ConstantForce)) {
        append_proxy_logf("EffectProxy::ConstantForce magnitude=%ld gain=%lu",
                          static_cast<long>(staged_state_.constant_magnitude),
                          static_cast<unsigned long>(staged_state_.effect_gain));
    } else if (is_guid_equal(guid_, GUID_RampForce)) {
        append_proxy_logf("EffectProxy::RampForce start=%ld end=%ld",
                          static_cast<long>(staged_state_.ramp_start),
                          static_cast<long>(staged_state_.ramp_end));
    } else if (is_guid_equal(guid_, GUID_Sine) || is_guid_equal(guid_, GUID_Square) ||
               is_guid_equal(guid_, GUID_Triangle) || is_guid_equal(guid_, GUID_SawtoothUp) ||
               is_guid_equal(guid_, GUID_SawtoothDown)) {
        append_proxy_logf("EffectProxy::Periodic magnitude=%lu offset=%ld period=%lu",
                          static_cast<unsigned long>(staged_state_.periodic_magnitude),
                          static_cast<long>(staged_state_.periodic_offset),
                          static_cast<unsigned long>(staged_state_.periodic_period));
    }
}

void EffectProxy::commit_staged(bool preserve_runtime) {
    const bool was_started = state_.started;
    const std::uint32_t previous_iterations = state_.iterations;
    const std::uint64_t previous_start_time = state_.start_time_us;

    state_ = staged_state_;
    if (preserve_runtime) {
        state_.started = was_started;
        state_.iterations = previous_iterations;
        state_.start_time_us = previous_start_time;
    } else {
        state_.started = false;
        state_.iterations = 1;
        state_.start_time_us = 0;
    }

    downloaded_ = true;
    dirty_since_download_ = false;
}

void EffectProxy::start_runtime(DWORD iterations) {
    state_.iterations = (iterations == 0) ? 1 : iterations;
    state_.started = true;
    state_.start_time_us = now_us();
}

void reset_payload_cache(bool& have_last_payload,
                         bool& last_sent_has_state,
                         wheelio_bridge::WheelStatePayload& last_payload) {
    have_last_payload = false;
    last_sent_has_state = false;
    last_payload = wheelio_bridge::WheelStatePayload{};
}

void force_stop_effects(EffectProxy* const* effects, int effect_count) {
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i]) {
            effects[i]->force_stop_runtime();
        }
    }
}

void force_stop_effects_except(EffectProxy* const* effects, int effect_count, EffectProxy* keep) {
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i] && effects[i] != keep) {
            effects[i]->force_stop_runtime();
        }
    }
}

void shift_effect_times(EffectProxy* const* effects, int effect_count, ULONGLONG delta_us) {
    if (delta_us == 0) {
        return;
    }
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i]) {
            effects[i]->shift_runtime_time(delta_us);
        }
    }
}

void remove_effect_from_table(EffectProxy** effects, int& effect_count, EffectProxy* effect) {
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i] == effect) {
            for (int j = i; j < effect_count - 1; ++j) {
                effects[j] = effects[j + 1];
            }
            effects[effect_count - 1] = nullptr;
            --effect_count;
            break;
        }
    }
}

bool effect_table_has_active_time_varying_effect(EffectProxy* const* effects, int effect_count) {
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i] && effects[i]->has_time_varying_force()) {
            return true;
        }
    }
    return false;
}

bool effect_table_needs_runtime_tick(EffectProxy* const* effects, int effect_count, ULONGLONG now) {
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i] && effects[i]->needs_runtime_tick(now)) {
            return true;
        }
    }
    return false;
}

void service_effect_table_runtime(EffectProxy* const* effects, int effect_count, ULONGLONG now) {
    for (int i = 0; i < effect_count; ++i) {
        if (effects[i]) {
            effects[i]->service_runtime_tick(now);
        }
    }
}

bool process_trigger_events(EffectProxy* const* effects,
                            int effect_count,
                            const DIDEVICEOBJECTDATA* data,
                            DWORD count) {
    bool changed = false;
    for (DWORD item = 0; data && item < count; ++item) {
        for (int effect = 0; effect < effect_count; ++effect) {
            if (effects[effect] && effects[effect]->handle_trigger_event(data[item].dwOfs, data[item].dwData)) {
                changed = true;
            }
        }
    }
    return changed;
}

wheelio_bridge::WheelStatePayload build_wheel_state_payload(
    EffectProxy* const* effects,
    int effect_count,
    DWORD ff_gain,
    DWORD autocenter_mode,
    DWORD ff_state,
    ULONGLONG now) {
    wheelio_bridge::DirectInputEffectView effect_views[kMaxEffects]{};
    std::size_t effect_view_count = 0;

    if ((ff_state & DIGFFS_PAUSED) != 0) {
        return wheelio_bridge::WheelStatePayload{};
    }

    service_effect_table_runtime(effects, effect_count, now);

    for (int i = 0; i < effect_count; ++i) {
        if (effects[i] && effect_view_count < kMaxEffects) {
            effect_views[effect_view_count++] = effects[i]->effect_view();
        }
    }

    return wheelio_bridge::build_directinput_payload(
        effect_views,
        effect_view_count,
        ff_gain,
        autocenter_mode == DIPROPAUTOCENTER_ON,
        (ff_state & DIGFFS_ACTUATORSOFF) != 0,
        now);
}

void send_payload_if_changed(const char* log_prefix,
                             const wheelio_bridge::WheelStatePayload& payload,
                             DWORD& ff_state,
                             bool& have_last_payload,
                             bool& last_sent_has_state,
                             wheelio_bridge::WheelStatePayload& last_payload) {
    const bool has_state =
        payload.autocenter_enabled || payload.custom_spring_enabled ||
        payload.damper_enabled || payload.constant_force_enabled;

    if (has_state) {
        ff_state &= ~DIGFFS_EMPTY;
        ff_state &= ~DIGFFS_STOPPED;
        const bool payload_changed =
            !have_last_payload || std::memcmp(&payload, &last_payload, sizeof(payload)) != 0;
        if (payload_changed) {
            append_proxy_logf(
                "%s spring=%u damper=%u constant=%u constant_mag=%d",
                log_prefix,
                static_cast<unsigned>(payload.custom_spring_enabled),
                static_cast<unsigned>(payload.damper_enabled),
                static_cast<unsigned>(payload.constant_force_enabled),
                static_cast<int>(payload.constant_force_magnitude));
            bridge_send_state(payload);
            last_payload = payload;
            have_last_payload = true;
        }
        last_sent_has_state = true;
        return;
    }

    ff_state |= DIGFFS_EMPTY | DIGFFS_STOPPED;
    have_last_payload = false;
    if (last_sent_has_state) {
        append_proxy_logf("%s stop_all", log_prefix);
        bridge_send_stop_all();
        last_sent_has_state = false;
        last_payload = wheelio_bridge::WheelStatePayload{};
    }
}

void rebuild_effect_state_and_send(const char* log_prefix,
                                   EffectProxy* const* effects,
                                   int effect_count,
                                   DWORD ff_gain,
                                   DWORD autocenter_mode,
                                   DWORD& ff_state,
                                   bool& have_last_payload,
                                   bool& last_sent_has_state,
                                   wheelio_bridge::WheelStatePayload& last_payload) {
    const ULONGLONG now = now_us();
    const auto payload = build_wheel_state_payload(effects, effect_count, ff_gain, autocenter_mode, ff_state, now);
    send_payload_if_changed(log_prefix, payload, ff_state, have_last_payload, last_sent_has_state, last_payload);
}

template <typename RebuildFn>
void apply_force_feedback_command(DWORD command,
                                  EffectProxy* const* effects,
                                  int effect_count,
                                  DWORD& ff_state,
                                  ULONGLONG& pause_started_us,
                                  bool& have_last_payload,
                                  bool& last_sent_has_state,
                                  wheelio_bridge::WheelStatePayload& last_payload,
                                  RebuildFn rebuild) {
    switch (command) {
        case DISFFC_RESET:
        case DISFFC_STOPALL:
            force_stop_effects(effects, effect_count);
            ff_state |= DIGFFS_STOPPED | DIGFFS_EMPTY;
            ff_state &= ~DIGFFS_PAUSED;
            pause_started_us = 0;
            bridge_send_stop_all();
            reset_payload_cache(have_last_payload, last_sent_has_state, last_payload);
            break;
        case DISFFC_PAUSE:
            if ((ff_state & DIGFFS_PAUSED) == 0) {
                pause_started_us = now_us();
            }
            ff_state |= DIGFFS_PAUSED;
            bridge_send_stop_all();
            reset_payload_cache(have_last_payload, last_sent_has_state, last_payload);
            break;
        case DISFFC_CONTINUE:
            if ((ff_state & DIGFFS_PAUSED) != 0 && pause_started_us != 0) {
                const ULONGLONG now = now_us();
                if (now > pause_started_us) {
                    shift_effect_times(effects, effect_count, now - pause_started_us);
                }
            }
            pause_started_us = 0;
            ff_state &= ~DIGFFS_PAUSED;
            rebuild();
            break;
        case DISFFC_SETACTUATORSON:
            ff_state |= DIGFFS_ACTUATORSON;
            ff_state &= ~DIGFFS_ACTUATORSOFF;
            rebuild();
            break;
        case DISFFC_SETACTUATORSOFF:
            ff_state |= DIGFFS_ACTUATORSOFF;
            ff_state &= ~DIGFFS_ACTUATORSON;
            bridge_send_stop_all();
            reset_payload_cache(have_last_payload, last_sent_has_state, last_payload);
            break;
        default:
            break;
    }
}

template <typename T>
DeviceProxyT<T>::DeviceProxyT(typename T::Device* inner, bool synthetic_force_feedback)
    : ref_count_(1), inner_(inner), effects_{}, effect_count_(0), ff_gain_(DI_FFNOMINALMAX),
      autocenter_mode_(DIPROPAUTOCENTER_ON), ff_state_(DIGFFS_EMPTY | DIGFFS_STOPPED | DIGFFS_ACTUATORSON | DIGFFS_POWERON),
      synthetic_force_feedback_(synthetic_force_feedback), advertises_force_feedback_(synthetic_force_feedback),
      last_sent_has_state_(false), have_last_payload_(false),
      last_periodic_rebuild_us_(0), pause_started_us_(0), last_payload_{},
      runtime_stop_event_(nullptr), runtime_thread_(nullptr) {
    InitializeCriticalSection(&state_lock_);
    if (synthetic_force_feedback_) {
        runtime_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (runtime_stop_event_) {
        runtime_thread_ = CreateThread(nullptr, 0, &DeviceProxyT<T>::runtime_thread_entry, this, 0, nullptr);
    }
}

template <typename T>
DeviceProxyT<T>::~DeviceProxyT() {
    if (runtime_stop_event_) {
        SetEvent(runtime_stop_event_);
    }
    if (runtime_thread_) {
        WaitForSingleObject(runtime_thread_, INFINITE);
        CloseHandle(runtime_thread_);
        runtime_thread_ = nullptr;
    }
    if (runtime_stop_event_) {
        CloseHandle(runtime_stop_event_);
        runtime_stop_event_ = nullptr;
    }
    DeleteCriticalSection(&state_lock_);
}

template <typename T>
ULONG STDMETHODCALLTYPE DeviceProxyT<T>::AddRef() {
    inner_->AddRef();
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}

template <typename T>
ULONG STDMETHODCALLTYPE DeviceProxyT<T>::Release() {
    inner_->Release();
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::QueryInterface(REFIID riid, LPVOID* out) {
    if (!out) {
        return E_POINTER;
    }

    if (is_guid_equal(riid, IID_IUnknown) || T::is_device_iid(riid)) {
        append_proxy_logf("DeviceProxy%s::QueryInterface returning wrapped device", T::suffix);
        *out = static_cast<typename T::Device*>(this);
        AddRef();
        return DI_OK;
    }

    return inner_->QueryInterface(riid, out);
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetCapabilities(LPDIDEVCAPS caps) {
    if (!caps) {
        return E_POINTER;
    }

    const HRESULT result = inner_->GetCapabilities(caps);
    if (FAILED(result)) {
        return result;
    }

    advertises_force_feedback_ = synthetic_force_feedback_ &&
                                  (is_game_controller_type(caps->dwDevType) || caps->dwAxes > 0);
    if (advertises_force_feedback_) {
        caps->dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE | DIDC_SATURATION |
                         DIDC_POSNEGCOEFFICIENTS | DIDC_POSNEGSATURATION | DIDC_DEADBAND;
        if (caps->dwSize >= sizeof(DIDEVCAPS)) {
            caps->dwFFSamplePeriod = 0;
            caps->dwFFMinTimeResolution = 0;
            caps->dwFFDriverVersion = 1;
        }
    }

    append_proxy_logf("GetCapabilities flags=0x%08lx axes=%lu buttons=%lu povs=%lu",
                      static_cast<unsigned long>(caps->dwFlags),
                      static_cast<unsigned long>(caps->dwAxes),
                      static_cast<unsigned long>(caps->dwButtons),
                      static_cast<unsigned long>(caps->dwPOVs));

    return result;
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::EnumObjects(typename T::EnumObjectsCallback callback, LPVOID ref, DWORD flags) {
    if (!callback) {
        return DIERR_INVALIDPARAM;
    }

    if (!synthetic_force_feedback_) {
        return inner_->EnumObjects(callback, ref, flags);
    }

    EnumObjectContext<T> context{};
    context.callback = callback;
    context.ref = ref;
    context.requested_flags = flags;
    context.actuator_only = (flags & DIDFT_FFACTUATOR) != 0;
    context.actuator_emitted = false;

    DWORD inner_flags = flags;
    if (context.actuator_only) {
        inner_flags &= ~DIDFT_FFACTUATOR;
        inner_flags |= DIDFT_AXIS;
    }

    const HRESULT result = inner_->EnumObjects(enum_objects_wrapper<T>, &context, inner_flags);
    if (FAILED(result)) {
        return result;
    }

    return context.actuator_only && !context.actuator_emitted ? DI_OK : result;
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetProperty(REFGUID prop, LPDIPROPHEADER header) {
    if (!header) {
        return E_POINTER;
    }

    append_proxy_logf("GetProperty %s", diprop_name(prop));

    if (!synthetic_force_feedback_) {
        return inner_->GetProperty(prop, header);
    }

    if (is_property_key(prop, 7)) {
        if (header->dwSize < sizeof(DIPROPDWORD)) {
            return DIERR_INVALIDPARAM;
        }
        auto* value = reinterpret_cast<LPDIPROPDWORD>(header);
        EnterCriticalSection(&state_lock_);
        value->dwData = ff_gain_;
        LeaveCriticalSection(&state_lock_);
        return DI_OK;
    }

    if (is_property_key(prop, 9)) {
        if (header->dwSize < sizeof(DIPROPDWORD)) {
            return DIERR_INVALIDPARAM;
        }
        auto* value = reinterpret_cast<LPDIPROPDWORD>(header);
        EnterCriticalSection(&state_lock_);
        value->dwData = autocenter_mode_;
        LeaveCriticalSection(&state_lock_);
        return DI_OK;
    }

    return inner_->GetProperty(prop, header);
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SetProperty(REFGUID prop, LPCDIPROPHEADER header) {
    if (!header) {
        return E_POINTER;
    }

    if (is_property_key(prop, 4) && header->dwSize >= sizeof(DIPROPRANGE)) {
        const auto* range = reinterpret_cast<const DIPROPRANGE*>(header);
        append_proxy_logf("SetProperty RANGE obj=0x%lx lMin=%ld lMax=%ld",
                          static_cast<unsigned long>(header->dwObj),
                          static_cast<long>(range->lMin), static_cast<long>(range->lMax));
    } else if (header->dwSize >= sizeof(DIPROPDWORD)) {
        append_proxy_logf("SetProperty %s dwData=%lu", diprop_name(prop),
                          static_cast<unsigned long>(reinterpret_cast<const DIPROPDWORD*>(header)->dwData));
    } else {
        append_proxy_logf("SetProperty %s", diprop_name(prop));
    }

    if (!synthetic_force_feedback_) {
        return inner_->SetProperty(prop, header);
    }

    if (is_property_key(prop, 7)) {
        if (header->dwSize < sizeof(DIPROPDWORD)) {
            return DIERR_INVALIDPARAM;
        }
        const DWORD requested_gain = reinterpret_cast<const DIPROPDWORD*>(header)->dwData;
        if (requested_gain > DI_FFNOMINALMAX) {
            return DIERR_INVALIDPARAM;
        }
        EnterCriticalSection(&state_lock_);
        ff_gain_ = requested_gain;
        rebuild_and_send();
        LeaveCriticalSection(&state_lock_);
        return DI_OK;
    }

    if (is_property_key(prop, 9)) {
        if (header->dwSize < sizeof(DIPROPDWORD)) {
            return DIERR_INVALIDPARAM;
        }
        EnterCriticalSection(&state_lock_);
        autocenter_mode_ = reinterpret_cast<const DIPROPDWORD*>(header)->dwData;
        rebuild_and_send();
        LeaveCriticalSection(&state_lock_);
        return DI_OK;
    }

    return inner_->SetProperty(prop, header);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::Acquire() {
    const HRESULT result = inner_->Acquire();
    append_proxy_logf("Acquire -> 0x%08lx", static_cast<unsigned long>(result));
    return result;
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::Unacquire() {
    if (synthetic_force_feedback_) {
        EnterCriticalSection(&state_lock_);
        bridge_send_stop_all();
        ff_state_ |= DIGFFS_STOPPED | DIGFFS_EMPTY;
        reset_payload_cache(have_last_payload_, last_sent_has_state_, last_payload_);
        LeaveCriticalSection(&state_lock_);
    }
    return inner_->Unacquire();
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetDeviceState(DWORD size, LPVOID data) {
    return inner_->GetDeviceState(size, data);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetDeviceData(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags) {
    const HRESULT result = inner_->GetDeviceData(size, data, inout, flags);
    if (synthetic_force_feedback_ && SUCCEEDED(result) && data && inout && *inout > 0) {
        EnterCriticalSection(&state_lock_);
        if (process_trigger_events(effects_, effect_count_, data, *inout)) {
            rebuild_and_send();
        }
        LeaveCriticalSection(&state_lock_);
    }
    return result;
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SetDataFormat(LPCDIDATAFORMAT format) { return inner_->SetDataFormat(format); }
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SetEventNotification(HANDLE handle) { return inner_->SetEventNotification(handle); }
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SetCooperativeLevel(HWND window, DWORD flags) { return inner_->SetCooperativeLevel(window, flags); }
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetObjectInfo(typename T::ObjectInstance* instance, DWORD object, DWORD how) {
    const HRESULT result = inner_->GetObjectInfo(instance, object, how);
    if (synthetic_force_feedback_ && SUCCEEDED(result) && instance && (instance->dwType & DIDFT_AXIS) != 0) {
        instance->dwFlags |= DIDOI_FFACTUATOR;
        instance->dwFFMaxForce = kReportedMaxForceNewtons;
        instance->dwFFForceResolution = 1024;
        append_proxy_logf("GetObjectInfo actuator axis object=0x%lx how=%lu max_force=%lu",
                          static_cast<unsigned long>(object), static_cast<unsigned long>(how),
                          static_cast<unsigned long>(instance->dwFFMaxForce));
    }
    return result;
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetDeviceInfo(typename T::DeviceInstance* instance) {
    const HRESULT result = inner_->GetDeviceInfo(instance);
    if (synthetic_force_feedback_ &&
        SUCCEEDED(result) &&
        instance &&
        instance->dwSize >= sizeof(typename T::DeviceInstance) &&
        advertises_force_feedback_) {
        instance->guidFFDriver = CLSID_DirectInputDevice8;
        instance->dwDevType = kSyntheticDrivingType;
        append_proxy_log("GetDeviceInfo injected guidFFDriver");
    }
    return result;
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::RunControlPanel(HWND window, DWORD flags) { return inner_->RunControlPanel(window, flags); }
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::Initialize(HINSTANCE instance, DWORD version, REFGUID guid) { return inner_->Initialize(instance, version, guid); }
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::EnumEffects(typename T::EnumEffectsCallback callback, LPVOID ref, DWORD type) {
    if (!callback) {
        return DIERR_INVALIDPARAM;
    }
    if (!synthetic_force_feedback_) {
        return inner_->EnumEffects(callback, ref, type);
    }

    append_proxy_logf("EnumEffects type=0x%08lx", static_cast<unsigned long>(type));

    for (int i = 0; i < supported_effect_count(); ++i) {
        const auto& effect = supported_effects()[i];
        if (!effect_matches_type(effect, type)) {
            continue;
        }

        typename T::EffectInfo info{};
        info.dwSize = sizeof(typename T::EffectInfo);
        const HRESULT fill_result = populate_effect_info(&info, effect);
        if (FAILED(fill_result)) {
            append_proxy_logf("EnumEffects failed to populate effect info: 0x%08lx",
                              static_cast<unsigned long>(fill_result));
            return fill_result;
        }

        append_proxy_logf("EnumEffects reporting %s", effect.name_a);

        if (callback(&info, ref) == DIENUM_STOP) {
            append_proxy_log("EnumEffects callback requested stop");
            break;
        }
    }

    return DI_OK;
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetEffectInfo(typename T::EffectInfo* info, REFGUID guid) {
    if (!synthetic_force_feedback_) {
        return inner_->GetEffectInfo(info, guid);
    }

    for (int i = 0; i < supported_effect_count(); ++i) {
        const auto& effect = supported_effects()[i];
        if (is_guid_equal(guid, *effect.guid)) {
            append_proxy_log("GetEffectInfo returning synthetic effect info");
            return populate_effect_info(info, effect);
        }
    }

    return DIERR_DEVICENOTREG;
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetForceFeedbackState(LPDWORD out) {
    if (!out) {
        return E_POINTER;
    }
    if (!synthetic_force_feedback_) {
        return inner_->GetForceFeedbackState(out);
    }
    EnterCriticalSection(&state_lock_);
    *out = ff_state_;
    LeaveCriticalSection(&state_lock_);
    return DI_OK;
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref, DWORD flags) {
    return inner_->EnumCreatedEffectObjects(callback, ref, flags);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::Escape(LPDIEFFESCAPE escape) { return inner_->Escape(escape); }
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::Poll() {
    const HRESULT result = inner_->Poll();
    if (!synthetic_force_feedback_) {
        return result;
    }
    const ULONGLONG now = now_us();
    EnterCriticalSection(&state_lock_);
    if ((ff_state_ & DIGFFS_PAUSED) == 0 &&
        has_active_time_varying_effect() &&
        (last_periodic_rebuild_us_ == 0 || (now - last_periodic_rebuild_us_) >= kPeriodicUpdateIntervalUs)) {
        rebuild_and_send();
        last_periodic_rebuild_us_ = now;
    }
    LeaveCriticalSection(&state_lock_);
    return result;
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SendDeviceData(DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags) {
    return inner_->SendDeviceData(size, data, inout, flags);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::EnumEffectsInFile(typename T::String file, LPDIENUMEFFECTSINFILECALLBACK callback, LPVOID ref, DWORD flags) {
    return inner_->EnumEffectsInFile(file, callback, ref, flags);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::WriteEffectToFile(typename T::String file, DWORD entries, LPDIFILEEFFECT effects, DWORD flags) {
    return inner_->WriteEffectToFile(file, entries, effects, flags);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::BuildActionMap(typename T::ActionFormat* format, typename T::String user, DWORD flags) {
    return inner_->BuildActionMap(format, user, flags);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SetActionMap(typename T::ActionFormat* format, typename T::String user, DWORD flags) {
    return inner_->SetActionMap(format, user, flags);
}
template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::GetImageInfo(typename T::ImageInfoHeader* header) { return inner_->GetImageInfo(header); }

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::CreateEffect(
    REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) {
    if (!out) {
        return E_POINTER;
    }
    if (!synthetic_force_feedback_) {
        return inner_->CreateEffect(guid, effect, out, outer);
    }
    (void)outer;

    IDirectInputEffect* inner_effect = nullptr;
    append_proxy_logf("CreateEffect called effect=%s", effect_guid_name(guid));
    HRESULT result = inner_->CreateEffect(guid, effect, &inner_effect, outer);
    if (FAILED(result) || !inner_effect) {
        inner_effect = nullptr;
        result = DI_OK;
        append_proxy_log("CreateEffect using software fallback");
    }

    auto* proxy = new EffectProxy(inner_effect, guid, this);
    EnterCriticalSection(&state_lock_);
    if (effect_count_ < kMaxEffects) {
        effects_[effect_count_++] = proxy;
    } else {
        append_proxy_log("effect table full, not tracking additional effect");
    }
    if (effect) {
        // SetParameters calls back into rebuild_and_send via the owner; the
        // device CS is re-entrant so holding it here is safe.
        proxy->SetParameters(effect, DIEP_ALLPARAMS);
    }
    LeaveCriticalSection(&state_lock_);
    *out = proxy;
    return result;
}

template <typename T>
HRESULT STDMETHODCALLTYPE DeviceProxyT<T>::SendForceFeedbackCommand(DWORD command) {
    if (!synthetic_force_feedback_) {
        return inner_->SendForceFeedbackCommand(command);
    }

    append_proxy_logf("SendForceFeedbackCommand command=0x%08lx",
                      static_cast<unsigned long>(command));
    HRESULT result = inner_->SendForceFeedbackCommand(command);
    if (FAILED(result)) {
        result = DI_OK;
    }

    EnterCriticalSection(&state_lock_);
    apply_force_feedback_command(
        command,
        effects_,
        effect_count_,
        ff_state_,
        pause_started_us_,
        have_last_payload_,
        last_sent_has_state_,
        last_payload_,
        [this]() { rebuild_and_send(); });
    LeaveCriticalSection(&state_lock_);

    return result;
}

template <typename T>
void DeviceProxyT<T>::remove_effect(EffectProxy* effect) {
    EnterCriticalSection(&state_lock_);
    remove_effect_from_table(effects_, effect_count_, effect);
    rebuild_and_send();
    LeaveCriticalSection(&state_lock_);
}

template <typename T>
void DeviceProxyT<T>::stop_effects_except(EffectProxy* keep) {
    EnterCriticalSection(&state_lock_);
    force_stop_effects_except(effects_, effect_count_, keep);
    LeaveCriticalSection(&state_lock_);
}

template <typename T>
bool DeviceProxyT<T>::has_active_time_varying_effect() const {
    EnterCriticalSection(&state_lock_);
    const bool active = effect_table_has_active_time_varying_effect(effects_, effect_count_);
    LeaveCriticalSection(&state_lock_);
    return active;
}

template <typename T>
bool DeviceProxyT<T>::has_runtime_tick_effect(ULONGLONG now) const {
    const bool active = effect_table_needs_runtime_tick(effects_, effect_count_, now);
    return active;
}

template <typename T>
DWORD WINAPI DeviceProxyT<T>::runtime_thread_entry(LPVOID parameter) {
    static_cast<DeviceProxyT<T>*>(parameter)->runtime_loop();
    return 0;
}

template <typename T>
void DeviceProxyT<T>::runtime_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    for (;;) {
        const DWORD wait_result = WaitForSingleObject(runtime_stop_event_, kRuntimeUpdateIntervalMs);
        if (wait_result != WAIT_TIMEOUT) {
            break;
        }

        const ULONGLONG now = now_us();
        EnterCriticalSection(&state_lock_);
        if ((ff_state_ & DIGFFS_PAUSED) == 0 && has_runtime_tick_effect(now)) {
            rebuild_effect_state_and_send(
                "runtime_tick",
                effects_,
                effect_count_,
                ff_gain_,
                autocenter_mode_,
                ff_state_,
                have_last_payload_,
                last_sent_has_state_,
                last_payload_);
            last_periodic_rebuild_us_ = now;
        }
        LeaveCriticalSection(&state_lock_);
    }
}

template <typename T>
void DeviceProxyT<T>::rebuild_and_send() {
    EnterCriticalSection(&state_lock_);
    rebuild_effect_state_and_send(
        "rebuild_and_send",
        effects_,
        effect_count_,
        ff_gain_,
        autocenter_mode_,
        ff_state_,
        have_last_payload_,
        last_sent_has_state_,
        last_payload_);
    LeaveCriticalSection(&state_lock_);
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_this_module = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Only request shutdown (sets a flag + wakes the worker). Do NOT call
        // the full shutdown()/join here: joining the worker under the loader
        // lock can deadlock, and the process is tearing down anyway.
        g_bridge_client.request_shutdown();
        InterlockedExchange(&g_bridge_announced, 0);
        g_this_module = nullptr;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    append_proxy_logf("DirectInput8Create called for %s", directinput_iid_name(riid));
    announce_bridge_connection();

    if (!out || !ensure_real_dinput_loaded()) {
        append_proxy_log("DirectInput8Create failed before real create");
        return E_FAIL;
    }

    LPVOID raw = nullptr;
    const HRESULT result = g_real_create(instance, version, riid, &raw, outer);
    if (FAILED(result) || !raw) {
        append_proxy_logf("real DirectInput8Create failed: 0x%08lx", static_cast<unsigned long>(result));
        return result;
    }

    if (is_guid_equal(riid, IID_IDirectInput8W)) {
        append_proxy_log("wrapping IDirectInput8W");
        *out = static_cast<IDirectInput8W*>(new DirectInputProxyT<DirectInputW>(reinterpret_cast<IDirectInput8W*>(raw)));
        return result;
    }

    if (is_guid_equal(riid, IID_IDirectInput8A)) {
        append_proxy_log("wrapping IDirectInput8A");
        *out = static_cast<IDirectInput8A*>(new DirectInputProxyT<DirectInputA>(reinterpret_cast<IDirectInput8A*>(raw)));
        return result;
    }

    append_proxy_log("passing DirectInput interface through without wrapping");
    *out = raw;
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    ensure_real_dinput_loaded();
    return g_real_can_unload ? g_real_can_unload() : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    ensure_real_dinput_loaded();
    return g_real_get_class_object ? g_real_get_class_object(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllRegisterServer() {
    ensure_real_dinput_loaded();
    return g_real_register_server ? g_real_register_server() : S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllUnregisterServer() {
    ensure_real_dinput_loaded();
    return g_real_unregister_server ? g_real_unregister_server() : S_OK;
}
