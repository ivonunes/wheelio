#pragma once

#include "wheelio_vr_shared.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Watches the shared buffer the OpenXR runtime keeps inside the CrossOver
// bottle, to tell whether a game is running in VR. The runtime maps a file
// under the bottle's Windows user profile; on this side that is an ordinary
// file, so we map the same pages. Frames themselves are read by the streamer.
class VRFrameReader {
public:
    struct Status {
        bool connected = false;
        // The game submitted frames within the last few seconds: it is running
        // in VR right now, not just a leftover file.
        bool game_running = false;
        std::string path;
    };

    ~VRFrameReader();

    // Looks for a published buffer and maps it. Cheap to call repeatedly; does
    // nothing while a valid mapping is already open. Never signals the runtime
    // to read frames back: that is the streamer's job.
    void poll();

    Status status() const;

private:
    void close_locked();
    bool open_locked(const std::string& path);

    mutable std::mutex mutex_;
    int fd_ = -1;
    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    std::string path_;

    // Liveness: when frames_submitted last changed, in this process's clock.
    std::uint64_t last_submitted_count_ = 0;
    std::uint64_t last_submitted_change_ns_ = 0;
};

// Every place a bottle could hold a published buffer, newest file first.
std::vector<std::string> discover_vr_shared_paths();
