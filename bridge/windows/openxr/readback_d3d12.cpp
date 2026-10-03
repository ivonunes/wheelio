#include "gpu_readback.hpp"
#include "runtime_log.hpp"

#include <cstring>
#include <new>

namespace {

constexpr std::uint32_t kMaxSlots = 4;

template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

// A committed buffer on the readback heap plus the footprint the copy uses.
struct ReadbackBuffer {
    ID3D12Resource* buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};

class D3D12Readback final : public GpuReadback {
public:
    D3D12Readback(ID3D12Device* device, ID3D12CommandQueue* queue) : device_(device), queue_(queue) {
        device_->AddRef();
        queue_->AddRef();
    }

    ~D3D12Readback() override {
        stop();
        release(queue_);
        release(device_);
    }

    bool start(std::uint32_t slots, std::uint32_t eye_width, std::uint32_t eye_height, DXGI_FORMAT format) override {
        if (slots > kMaxSlots) {
            return false;
        }
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            runtime_log("frame export: CreateFence failed");
            return false;
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) {
            stop();
            return false;
        }
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[slot])))) {
                runtime_log("frame export: CreateCommandAllocator failed");
                stop();
                return false;
            }
            for (ReadbackBuffer& target : targets_[slot]) {
                if (!create_buffer(eye_width, eye_height, format, target)) {
                    stop();
                    return false;
                }
            }
        }
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0], nullptr,
                                              IID_PPV_ARGS(&list_)))) {
            runtime_log("frame export: CreateCommandList failed");
            stop();
            return false;
        }
        list_->Close();
        return true;
    }

    void stop() override {
        wait_for(last_signalled_);
        release(list_);
        for (auto& pair : targets_) {
            for (ReadbackBuffer& target : pair) {
                release(target.buffer);
            }
        }
        for (ID3D12CommandAllocator*& allocator : allocators_) {
            release(allocator);
        }
        if (fence_event_) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        release(fence_);
        for (std::uint64_t& value : slot_fence_) {
            value = 0;
        }
        last_signalled_ = 0;
    }

    bool copy(std::uint32_t slot, const EyeSource eyes[2]) override {
        if (!eyes[0].resource12 || !eyes[1].resource12) {
            return false;
        }
        // The allocator is reused once the GPU has finished this slot's
        // previous copy, which the caller guarantees by mapping (and so
        // waiting for) the slot before issuing a new copy into it.
        if (FAILED(allocators_[slot]->Reset()) || FAILED(list_->Reset(allocators_[slot], nullptr))) {
            runtime_log("frame export: command list reset failed");
            return false;
        }
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            record_copy(eyes[eye], targets_[slot][eye]);
        }
        if (FAILED(list_->Close())) {
            runtime_log("frame export: command list close failed");
            return false;
        }
        ID3D12CommandList* lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        slot_fence_[slot] = ++last_signalled_;
        queue_->Signal(fence_, slot_fence_[slot]);
        return true;
    }

    bool map(std::uint32_t slot, bool wait, MappedEye out[2]) override {
        if (fence_->GetCompletedValue() < slot_fence_[slot]) {
            if (!wait) {
                return false;
            }
            wait_for(slot_fence_[slot]);
        }
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            ReadbackBuffer& target = targets_[slot][eye];
            const D3D12_RANGE range{0, static_cast<SIZE_T>(target.footprint.Footprint.RowPitch) *
                                           target.footprint.Footprint.Height};
            void* data = nullptr;
            if (FAILED(target.buffer->Map(0, &range, &data))) {
                for (std::uint32_t done = 0; done < eye; ++done) {
                    unmap_one(targets_[slot][done]);
                }
                return false;
            }
            out[eye].data = static_cast<const std::uint8_t*>(data);
            out[eye].row_pitch = target.footprint.Footprint.RowPitch;
        }
        return true;
    }

    void unmap(std::uint32_t slot) override {
        for (ReadbackBuffer& target : targets_[slot]) {
            unmap_one(target);
        }
    }

    bool snapshot(const EyeSource& eye, std::uint32_t width, std::uint32_t height, std::uint8_t* out) override {
        if (!eye.resource12 || !list_) {
            return false;
        }
        const D3D12_RESOURCE_DESC desc = eye.resource12->GetDesc();
        ReadbackBuffer target;
        if (!create_buffer(static_cast<std::uint32_t>(desc.Width), desc.Height, desc.Format, target)) {
            return false;
        }
        bool ok = false;
        wait_for(last_signalled_);
        if (SUCCEEDED(allocators_[0]->Reset()) && SUCCEEDED(list_->Reset(allocators_[0], nullptr))) {
            record_copy(eye, target);
            list_->Close();
            ID3D12CommandList* lists[] = {list_};
            queue_->ExecuteCommandLists(1, lists);
            queue_->Signal(fence_, ++last_signalled_);
            wait_for(last_signalled_);
            void* data = nullptr;
            if (SUCCEEDED(target.buffer->Map(0, nullptr, &data))) {
                for (std::uint32_t row = 0; row < height; ++row) {
                    std::memcpy(out + row * static_cast<std::size_t>(width) * 4,
                                static_cast<const std::uint8_t*>(data) +
                                    row * static_cast<std::size_t>(target.footprint.Footprint.RowPitch),
                                static_cast<std::size_t>(width) * 4);
                }
                unmap_one(target);
                ok = true;
            }
        }
        release(target.buffer);
        return ok;
    }

private:
    bool create_buffer(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format, ReadbackBuffer& out) {
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width;
        texture.Height = height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = format;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        UINT64 total = 0;
        device_->GetCopyableFootprints(&texture, 0, 1, 0, &out.footprint, nullptr, nullptr, &total);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&out.buffer));
        if (FAILED(hr)) {
            runtime_logf("frame export: readback CreateCommittedResource failed 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    // The swapchain image is in the render-target state whenever the game
    // holds no acquired image (the OpenXR D3D12 contract), so the copy moves
    // it out and back.
    void record_copy(const EyeSource& eye, const ReadbackBuffer& target) {
        const D3D12_RESOURCE_DESC desc = eye.resource12->GetDesc();
        const UINT subresource = eye.array_index * desc.MipLevels;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = eye.resource12;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list_->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = eye.resource12;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = target.buffer;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = target.footprint;
        list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list_->ResourceBarrier(1, &barrier);
    }

    void wait_for(std::uint64_t value) {
        if (!fence_ || value == 0 || fence_->GetCompletedValue() >= value) {
            return;
        }
        if (SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
            WaitForSingleObject(fence_event_, INFINITE);
        }
    }

    static void unmap_one(ReadbackBuffer& target) {
        const D3D12_RANGE nothing_written{0, 0};
        target.buffer->Unmap(0, &nothing_written);
    }

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12CommandAllocator* allocators_[kMaxSlots] = {};
    ReadbackBuffer targets_[kMaxSlots][2];
    std::uint64_t slot_fence_[kMaxSlots] = {};
    std::uint64_t last_signalled_ = 0;
};

alignas(D3D12Readback) unsigned char g_storage[sizeof(D3D12Readback)];

}  // namespace

GpuReadback* create_d3d12_readback(ID3D12Device* device, ID3D12CommandQueue* queue) {
    return device && queue ? new (g_storage) D3D12Readback(device, queue) : nullptr;
}
