// Stand-in for a game: drives the Wheelio OpenXR runtime through a full
// session on D3D11 or D3D12 without an OpenXR loader, so both graphics paths
// can be checked inside a CrossOver bottle with no game installed.
//
//   wheelio_xr_smoke.exe [--d3d12] [--frames N] [--dll path\to\wheelio_openxr.dll]
//
// Each frame clears the left eye to a warm and the right eye to a cool colour
// that shifts over time, with a white block in the top-left corner so the
// orientation of what arrives on the mac side is unambiguous. Run with
// WHEELIO_XR_DUMP=1 to also get the runtime's own PNG dumps.

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

PFN_xrGetInstanceProcAddr g_gipa = nullptr;

template <typename T>
T proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(g_gipa(instance, name, &function)) || !function) {
        std::fprintf(stderr, "missing runtime function %s\n", name);
        std::exit(2);
    }
    return reinterpret_cast<T>(function);
}

#define CHECK_XR(call)                                                          \
    do {                                                                        \
        const XrResult result_ = (call);                                        \
        if (XR_FAILED(result_)) {                                               \
            std::fprintf(stderr, "%s failed: %d\n", #call, static_cast<int>(result_)); \
            std::exit(3);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_HR(call)                                                          \
    do {                                                                        \
        const HRESULT hr_ = (call);                                             \
        if (FAILED(hr_)) {                                                      \
            std::fprintf(stderr, "%s failed: 0x%08lx\n", #call, static_cast<unsigned long>(hr_)); \
            std::exit(4);                                                       \
        }                                                                       \
    } while (0)

constexpr uint32_t kImageCount = 3;
constexpr uint32_t kBlockSize = 256;

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D11Texture2D* textures11[kImageCount] = {};
    ID3D11RenderTargetView* views11[kImageCount] = {};
    ID3D12Resource* textures12[kImageCount] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE views12[kImageCount] = {};
};

struct Gpu {
    bool d3d12 = false;
    ID3D11Device* device11 = nullptr;
    ID3D11DeviceContext* context11 = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue12 = nullptr;
    ID3D12CommandAllocator* allocator12 = nullptr;
    ID3D12GraphicsCommandList* list12 = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    UINT rtv_size = 0;
    UINT rtv_used = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
};

void create_device(Gpu& gpu) {
    if (gpu.d3d12) {
        CHECK_HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device12)));
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK_HR(gpu.device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue12)));
        CHECK_HR(gpu.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator12)));
        CHECK_HR(gpu.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator12, nullptr,
                                                 IID_PPV_ARGS(&gpu.list12)));
        gpu.list12->Close();
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 2 * kImageCount;
        CHECK_HR(gpu.device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&gpu.rtv_heap)));
        gpu.rtv_size = gpu.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CHECK_HR(gpu.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)));
        gpu.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    CHECK_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                               &gpu.device11, nullptr, &gpu.context11));
}

void wait_gpu(Gpu& gpu) {
    const UINT64 value = ++gpu.fence_value;
    gpu.queue12->Signal(gpu.fence, value);
    if (gpu.fence->GetCompletedValue() < value) {
        gpu.fence->SetEventOnCompletion(value, gpu.fence_event);
        WaitForSingleObject(gpu.fence_event, INFINITE);
    }
}

void create_eye(Gpu& gpu, XrInstance instance, XrSession session, int64_t format, uint32_t width, uint32_t height,
                Eye& eye) {
    auto xrCreateSwapchain = proc<PFN_xrCreateSwapchain>(instance, "xrCreateSwapchain");
    auto xrEnumerateSwapchainImages = proc<PFN_xrEnumerateSwapchainImages>(instance, "xrEnumerateSwapchainImages");

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    CHECK_XR(xrCreateSwapchain(session, &info, &eye.swapchain));
    eye.width = width;
    eye.height = height;

    uint32_t count = 0;
    CHECK_XR(xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr));
    if (count != kImageCount) {
        std::fprintf(stderr, "expected %u swapchain images, got %u\n", kImageCount, count);
        std::exit(5);
    }
    if (gpu.d3d12) {
        XrSwapchainImageD3D12KHR images[kImageCount];
        for (auto& image : images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        }
        CHECK_XR(xrEnumerateSwapchainImages(eye.swapchain, kImageCount, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(images)));
        for (uint32_t i = 0; i < kImageCount; ++i) {
            eye.textures12[i] = images[i].texture;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = gpu.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(gpu.rtv_used++) * gpu.rtv_size;
            gpu.device12->CreateRenderTargetView(eye.textures12[i], nullptr, handle);
            eye.views12[i] = handle;
        }
        return;
    }
    XrSwapchainImageD3D11KHR images[kImageCount];
    for (auto& image : images) {
        image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    }
    CHECK_XR(xrEnumerateSwapchainImages(eye.swapchain, kImageCount, &count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(images)));
    for (uint32_t i = 0; i < kImageCount; ++i) {
        eye.textures11[i] = images[i].texture;
        CHECK_HR(gpu.device11->CreateRenderTargetView(eye.textures11[i], nullptr, &eye.views11[i]));
    }
}

void paint(Gpu& gpu, const Eye& eye, uint32_t image, const float colour[4]) {
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (gpu.d3d12) {
        const D3D12_RECT block{0, 0, static_cast<LONG>(kBlockSize), static_cast<LONG>(kBlockSize)};
        gpu.allocator12->Reset();
        gpu.list12->Reset(gpu.allocator12, nullptr);
        gpu.list12->ClearRenderTargetView(eye.views12[image], colour, 0, nullptr);
        gpu.list12->ClearRenderTargetView(eye.views12[image], white, 1, &block);
        gpu.list12->Close();
        ID3D12CommandList* lists[] = {gpu.list12};
        gpu.queue12->ExecuteCommandLists(1, lists);
        // One command allocator: wait so the next frame can reset it.
        wait_gpu(gpu);
        return;
    }
    const D3D11_RECT block{0, 0, static_cast<LONG>(kBlockSize), static_cast<LONG>(kBlockSize)};
    gpu.context11->ClearRenderTargetView(eye.views11[image], colour);
    ID3D11DeviceContext1* context1 = nullptr;
    if (SUCCEEDED(gpu.context11->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
        context1->ClearView(eye.views11[image], white, &block, 1);
        context1->Release();
    }
}

// Polls until the session reaches `wanted`, forwarding state changes.
void wait_for_state(XrInstance instance, XrSessionState wanted, XrSessionState& current) {
    auto xrPollEvent = proc<PFN_xrPollEvent>(instance, "xrPollEvent");
    for (int spins = 0; spins < 1000 && current != wanted; ++spins) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            Sleep(5);
            continue;
        }
        CHECK_XR(result);
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            current = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event)->state;
            std::printf("session state %d\n", static_cast<int>(current));
        }
    }
    if (current != wanted) {
        std::fprintf(stderr, "session never reached state %d\n", static_cast<int>(wanted));
        std::exit(6);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Gpu gpu;
    uint32_t frames = 300;
    char dll_path[MAX_PATH] = "wheelio_openxr.dll";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--d3d12") == 0) {
            gpu.d3d12 = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--dll") == 0 && i + 1 < argc) {
            std::snprintf(dll_path, sizeof(dll_path), "%s", argv[++i]);
        } else {
            std::fprintf(stderr, "usage: %s [--d3d12] [--frames N] [--dll path]\n", argv[0]);
            return 1;
        }
    }
    std::printf("wheelio_xr_smoke: %s, %u frames, runtime %s\n", gpu.d3d12 ? "D3D12" : "D3D11", frames, dll_path);

    HMODULE runtime = LoadLibraryA(dll_path);
    if (!runtime) {
        std::fprintf(stderr, "cannot load %s (error %lu)\n", dll_path, GetLastError());
        return 2;
    }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        reinterpret_cast<void*>(GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface")));
    if (!negotiate) {
        std::fprintf(stderr, "runtime has no xrNegotiateLoaderRuntimeInterface\n");
        return 2;
    }
    XrNegotiateLoaderInfo loader_info{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
                                      sizeof(XrNegotiateLoaderInfo)};
    loader_info.minInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);
    XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION,
                                      sizeof(XrNegotiateRuntimeRequest)};
    CHECK_XR(negotiate(&loader_info, &request));
    g_gipa = request.getInstanceProcAddr;

    auto xrCreateInstance = proc<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance");
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instance_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "wheelio_xr_smoke");
    instance_info.applicationInfo.apiVersion = request.runtimeApiVersion;
    const char* extension = gpu.d3d12 ? XR_KHR_D3D12_ENABLE_EXTENSION_NAME : XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = &extension;
    XrInstance instance = XR_NULL_HANDLE;
    CHECK_XR(xrCreateInstance(&instance_info, &instance));

    auto xrGetSystem = proc<PFN_xrGetSystem>(instance, "xrGetSystem");
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    CHECK_XR(xrGetSystem(instance, &system_info, &system));

    if (gpu.d3d12) {
        auto requirements = proc<PFN_xrGetD3D12GraphicsRequirementsKHR>(instance, "xrGetD3D12GraphicsRequirementsKHR");
        XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        CHECK_XR(requirements(instance, system, &reqs));
    } else {
        auto requirements = proc<PFN_xrGetD3D11GraphicsRequirementsKHR>(instance, "xrGetD3D11GraphicsRequirementsKHR");
        XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK_XR(requirements(instance, system, &reqs));
    }
    create_device(gpu);

    auto xrCreateSession = proc<PFN_xrCreateSession>(instance, "xrCreateSession");
    XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding11.device = gpu.device11;
    XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding12.device = gpu.device12;
    binding12.queue = gpu.queue12;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = gpu.d3d12 ? static_cast<const void*>(&binding12) : static_cast<const void*>(&binding11);
    session_info.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    CHECK_XR(xrCreateSession(instance, &session_info, &session));

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    wait_for_state(instance, XR_SESSION_STATE_READY, state);
    auto xrBeginSession = proc<PFN_xrBeginSession>(instance, "xrBeginSession");
    XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    CHECK_XR(xrBeginSession(session, &begin_info));

    auto xrEnumerateViewConfigurationViews =
        proc<PFN_xrEnumerateViewConfigurationViews>(instance, "xrEnumerateViewConfigurationViews");
    XrViewConfigurationView config_views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;
    CHECK_XR(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &view_count, config_views));

    auto xrEnumerateSwapchainFormats = proc<PFN_xrEnumerateSwapchainFormats>(instance, "xrEnumerateSwapchainFormats");
    int64_t formats[16] = {};
    uint32_t format_count = 0;
    CHECK_XR(xrEnumerateSwapchainFormats(session, 16, &format_count, formats));
    std::printf("using swapchain format %lld, %ux%u per eye\n", static_cast<long long>(formats[0]),
                config_views[0].recommendedImageRectWidth, config_views[0].recommendedImageRectHeight);

    Eye eyes[2];
    for (uint32_t v = 0; v < 2; ++v) {
        create_eye(gpu, instance, session, formats[0], config_views[v].recommendedImageRectWidth,
                   config_views[v].recommendedImageRectHeight, eyes[v]);
    }

    auto xrCreateReferenceSpace = proc<PFN_xrCreateReferenceSpace>(instance, "xrCreateReferenceSpace");
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    CHECK_XR(xrCreateReferenceSpace(session, &space_info, &space));

    auto xrWaitFrame = proc<PFN_xrWaitFrame>(instance, "xrWaitFrame");
    auto xrBeginFrame = proc<PFN_xrBeginFrame>(instance, "xrBeginFrame");
    auto xrEndFrame = proc<PFN_xrEndFrame>(instance, "xrEndFrame");
    auto xrLocateViews = proc<PFN_xrLocateViews>(instance, "xrLocateViews");
    auto xrAcquireSwapchainImage = proc<PFN_xrAcquireSwapchainImage>(instance, "xrAcquireSwapchainImage");
    auto xrWaitSwapchainImage = proc<PFN_xrWaitSwapchainImage>(instance, "xrWaitSwapchainImage");
    auto xrReleaseSwapchainImage = proc<PFN_xrReleaseSwapchainImage>(instance, "xrReleaseSwapchainImage");

    const ULONGLONG started = GetTickCount64();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        CHECK_XR(xrWaitFrame(session, nullptr, &frame_state));
        CHECK_XR(xrBeginFrame(session, nullptr));

        XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
        locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate_info.displayTime = frame_state.predictedDisplayTime;
        locate_info.space = space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        CHECK_XR(xrLocateViews(session, &locate_info, &view_state, 2, &view_count, views));

        const float phase = static_cast<float>(frame % 144) / 144.0f;
        XrCompositionLayerProjectionView projection_views[2];
        for (uint32_t v = 0; v < 2; ++v) {
            uint32_t image = 0;
            CHECK_XR(xrAcquireSwapchainImage(eyes[v].swapchain, nullptr, &image));
            XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait_info.timeout = XR_INFINITE_DURATION;
            CHECK_XR(xrWaitSwapchainImage(eyes[v].swapchain, &wait_info));
            const float colour[4] = {v == 0 ? 0.8f : 0.1f, 0.2f + 0.6f * phase, v == 0 ? 0.1f : 0.8f, 1.0f};
            paint(gpu, eyes[v], image, colour);
            CHECK_XR(xrReleaseSwapchainImage(eyes[v].swapchain, nullptr));

            projection_views[v] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projection_views[v].pose = views[v].pose;
            projection_views[v].fov = views[v].fov;
            projection_views[v].subImage.swapchain = eyes[v].swapchain;
            projection_views[v].subImage.imageRect.extent.width = static_cast<int32_t>(eyes[v].width);
            projection_views[v].subImage.imageRect.extent.height = static_cast<int32_t>(eyes[v].height);
        }

        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = 2;
        layer.views = projection_views;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = frame_state.predictedDisplayTime;
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end_info.layerCount = 1;
        end_info.layers = layers;
        CHECK_XR(xrEndFrame(session, &end_info));

        if ((frame + 1) % 72 == 0) {
            std::printf("frame %u, %.1f fps\n", frame + 1,
                        1000.0 * (frame + 1) / static_cast<double>(GetTickCount64() - started + 1));
        }
    }

    auto xrRequestExitSession = proc<PFN_xrRequestExitSession>(instance, "xrRequestExitSession");
    auto xrEndSession = proc<PFN_xrEndSession>(instance, "xrEndSession");
    auto xrDestroySession = proc<PFN_xrDestroySession>(instance, "xrDestroySession");
    auto xrDestroyInstance = proc<PFN_xrDestroyInstance>(instance, "xrDestroyInstance");
    CHECK_XR(xrRequestExitSession(session));
    wait_for_state(instance, XR_SESSION_STATE_STOPPING, state);
    CHECK_XR(xrEndSession(session));
    CHECK_XR(xrDestroySession(session));
    CHECK_XR(xrDestroyInstance(instance));
    std::printf("done\n");
    return 0;
}
