#include "gpu_readback.hpp"
#include "runtime_log.hpp"

#include <d3d11_4.h>

#include <cstring>
#include <new>

namespace {

constexpr std::uint32_t kMaxSlots = 4;

// Holds the immediate context for the duration of a scope when the device is
// multithread-protected.
struct ContextGuard {
    explicit ContextGuard(ID3D11Multithread* multithread) : multithread_(multithread) {
        if (multithread_) {
            multithread_->Enter();
        }
    }
    ~ContextGuard() {
        if (multithread_) {
            multithread_->Leave();
        }
    }
    ID3D11Multithread* multithread_;
};

template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

class D3D11Readback final : public GpuReadback {
public:
    explicit D3D11Readback(ID3D11Device* device) : device_(device) {
        device_->AddRef();
        device_->GetImmediateContext(&context_);
        // The game submits frames on its own thread while rendering continues
        // on another; D3D11 immediate contexts are not thread-safe without this.
        if (SUCCEEDED(context_->QueryInterface(IID_ID3D11Multithread, reinterpret_cast<void**>(&multithread_))) &&
            multithread_) {
            multithread_->SetMultithreadProtected(TRUE);
        } else {
            runtime_log("frame export: ID3D11Multithread unavailable, context access is unguarded");
        }
    }

    ~D3D11Readback() override {
        stop();
        release(multithread_);
        release(context_);
        release(device_);
    }

    bool start(std::uint32_t slots, std::uint32_t eye_width, std::uint32_t eye_height, DXGI_FORMAT format) override {
        if (slots > kMaxSlots) {
            return false;
        }

        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            for (ID3D11Texture2D*& staging : staging_[slot]) {
                if (!create_staging(eye_width, eye_height, format, staging)) {
                    stop();
                    return false;
                }
            }
        }
        return true;
    }

    void stop() override {
        for (auto& pair : staging_) {
            for (ID3D11Texture2D*& staging : pair) {
                release(staging);
            }
        }
    }

    bool copy(std::uint32_t slot, const EyeSource eyes[2]) override {
        ContextGuard guard(multithread_);
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            if (!eyes[eye].texture11) {
                return false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            eyes[eye].texture11->GetDesc(&desc);
            const UINT source = D3D11CalcSubresource(0, eyes[eye].array_index, desc.MipLevels);
            context_->CopySubresourceRegion(staging_[slot][eye], 0, 0, 0, 0, eyes[eye].texture11, source, nullptr);
        }
        // Start the copies now rather than at the game's next flush, so they
        // have the whole next frame to land before we try to map them.
        context_->Flush();
        return true;
    }

    bool map(std::uint32_t slot, bool wait, MappedEye out[2]) override {
        ContextGuard guard(multithread_);
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr =
                context_->Map(staging_[slot][eye], 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (FAILED(hr)) {
                for (std::uint32_t done = 0; done < eye; ++done) {
                    context_->Unmap(staging_[slot][done], 0);
                }
                return false;
            }
            out[eye].data = static_cast<const std::uint8_t*>(mapped.pData);
            out[eye].row_pitch = mapped.RowPitch;
        }
        return true;
    }

    void unmap(std::uint32_t slot) override {
        ContextGuard guard(multithread_);
        for (ID3D11Texture2D* staging : staging_[slot]) {
            context_->Unmap(staging, 0);
        }
    }

    bool snapshot(const EyeSource& eye, std::uint32_t width, std::uint32_t height, std::uint8_t* out) override {
        if (!eye.texture11) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        eye.texture11->GetDesc(&desc);
        ID3D11Texture2D* staging = nullptr;
        if (!create_staging(desc.Width, desc.Height, desc.Format, staging)) {
            return false;
        }
        ContextGuard guard(multithread_);
        context_->CopySubresourceRegion(staging, 0, 0, 0, 0, eye.texture11,
                                        D3D11CalcSubresource(0, eye.array_index, desc.MipLevels), nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const bool ok = SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            for (std::uint32_t row = 0; row < height; ++row) {
                std::memcpy(out + row * static_cast<std::size_t>(width) * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) + row * static_cast<std::size_t>(mapped.RowPitch),
                            static_cast<std::size_t>(width) * 4);
            }
            context_->Unmap(staging, 0);
        }
        staging->Release();
        return ok;
    }

private:
    bool create_staging(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format, ID3D11Texture2D*& out) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &out);
        if (FAILED(hr)) {
            runtime_logf("frame export: staging CreateTexture2D failed 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Multithread* multithread_ = nullptr;
    ID3D11Texture2D* staging_[kMaxSlots][2] = {};
};

alignas(D3D11Readback) unsigned char g_storage[sizeof(D3D11Readback)];

}  // namespace

GpuReadback* create_d3d11_readback(ID3D11Device* device) {
    return device ? new (g_storage) D3D11Readback(device) : nullptr;
}
