#pragma once

#include "gpu_readback.hpp"
#include "wheelio_vr_shared.hpp"

#include <windows.h>
#include <cstdint>

// Copies the game's rendered eye textures out of the GPU (through a
// GpuReadback for the session's graphics API) and publishes them into the
// shared buffer the mac app reads.
//
// The GPU copy for frame N is issued during xrEndFrame(N), but the CPU only
// maps it two frames later. Mapping the texture we just wrote stalls until the
// GPU catches up (phase 0 measured 5-73 ms of it), and that stall lands
// squarely on the game's submit thread.
class FrameExporter {
public:
    struct ViewInfo {
        EyeSource source;
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float position[3] = {0.0f, 0.0f, 0.0f};
        float fov[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // left, right, up, down
        std::uint64_t tracking_timestamp_ns = 0;
    };

    // `readback` belongs to the session and outlives the exporter. Returns
    // false if the shared file could not be created.
    bool start(GpuReadback* readback, std::uint32_t eye_width, std::uint32_t eye_height, DXGI_FORMAT format);
    void stop();

    bool started() const { return started_; }

    // True while the mac app is reading. When false the caller should skip the
    // whole export so the game pays nothing. `now_ns` is the runtime's own
    // monotonic clock.
    bool reader_present(std::uint64_t now_ns);

    // Latest head pose and optics written by the mac app. Returns false when
    // the buffer is not mapped yet or the host has never written.
    bool read_host_state(wheelio_vr::HostState& out) const;

    // Counts a submitted frame, read back or not.
    void note_frame_submitted();

    // Issues the copy for this frame and publishes the frame captured two
    // frames ago. Safe to call from the frame-submit thread only.
    void submit_frame(std::uint32_t frame_id, std::uint64_t predicted_display_time_ns,
                      std::uint64_t submitted_at_unix_ns, const ViewInfo views[2]);

private:
    static constexpr std::uint32_t kPendingCount = 3;

    struct Pending {
        bool in_flight = false;
        std::uint32_t frame_id = 0;
        std::uint64_t predicted_display_time_ns = 0;
        std::uint64_t submitted_at_unix_ns = 0;
        ViewInfo views[2];
    };

    bool create_shared_file(std::uint32_t eye_width, std::uint32_t eye_height, DXGI_FORMAT format);
    // `wait` blocks until the GPU copy has landed; otherwise a copy still in
    // flight makes this return false and leaves the frame pending.
    bool publish(Pending& pending, bool wait);

    std::uint64_t published_ = 0;
    std::uint64_t waited_ = 0;
    std::uint64_t wait_total_us_ = 0;

    bool started_ = false;
    GpuReadback* readback_ = nullptr;

    std::uint32_t eye_width_ = 0;
    std::uint32_t eye_height_ = 0;
    std::uint32_t eye_stride_ = 0;
    std::uint32_t eye_size_ = 0;

    // Last heartbeat value seen and when (runtime clock) it last changed.
    std::uint64_t last_heartbeat_ = 0;
    std::uint64_t last_heartbeat_change_ns_ = 0;

    Pending pending_[kPendingCount];
    std::uint32_t next_pending_ = 0;
    std::uint32_t next_slot_ = 0;

    HANDLE file_ = nullptr;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    char file_path_[MAX_PATH] = {};
};
