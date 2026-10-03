#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <cstdint>

// The graphics-API-specific half of the frame export: copying a rendered eye
// texture into CPU-readable memory. One implementation per API the runtime
// accepts a graphics binding for; FrameExporter drives whichever the game's
// session uses and never touches D3D itself.
//
// `slot` indexes a small ring of readback targets so the copy for one frame
// can land while the next is issued.

// One eye of a swapchain image. Exactly one of the two pointers is set.
struct EyeSource {
    ID3D11Texture2D* texture11 = nullptr;
    ID3D12Resource* resource12 = nullptr;
    std::uint32_t array_index = 0;

    bool valid() const { return texture11 != nullptr || resource12 != nullptr; }
};

struct MappedEye {
    const std::uint8_t* data = nullptr;
    std::uint32_t row_pitch = 0;
};

class GpuReadback {
public:
    virtual ~GpuReadback() = default;

    // Creates `slots` rings of two readback targets of the given size.
    virtual bool start(std::uint32_t slots, std::uint32_t eye_width, std::uint32_t eye_height, DXGI_FORMAT format) = 0;
    virtual void stop() = 0;

    // Queues the GPU copy of both eyes into `slot`.
    virtual bool copy(std::uint32_t slot, const EyeSource eyes[2]) = 0;

    // Maps both eyes of `slot` for reading. Without `wait`, a copy still in
    // flight makes this return false and map nothing.
    virtual bool map(std::uint32_t slot, bool wait, MappedEye out[2]) = 0;
    virtual void unmap(std::uint32_t slot) = 0;

    // Blocking one-off readback of a single eye into `out`, `width * 4` bytes
    // per row with no padding. Used for PNG dumps only, so it may be slow.
    virtual bool snapshot(const EyeSource& eye, std::uint32_t width, std::uint32_t height, std::uint8_t* out) = 0;
};

// Each lives in static storage (the runtime has one session at a time) so
// nothing here goes through the heap: libstdc++'s operator new drags its
// exception machinery, and with it libwinpthread, into the DLL's imports.
// Finish with an explicit destructor call, never delete.
GpuReadback* create_d3d11_readback(ID3D11Device* device);
GpuReadback* create_d3d12_readback(ID3D12Device* device, ID3D12CommandQueue* queue);
