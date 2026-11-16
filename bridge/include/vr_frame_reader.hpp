#pragma once

#include "vr_shared.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Watches the shared buffer the OpenXR runtime keeps inside a game's Wine
// environment, to tell whether a game is running in VR. The runtime maps a
// file in the environment's temporary folder; on this side that is an
// ordinary file, so we map the same pages. Frames themselves are read by the
// streamer.
class VRFrameReader {
public:
    struct Status {
        bool connected = false;
        // The game submitted frames within the last few seconds: it is running
        // in VR right now, not just a leftover file.
        bool game_running = false;
        // The frames file, and the size of each eye in it, for the streamer.
        std::string path;
        std::uint32_t eye_width = 0;
        std::uint32_t eye_height = 0;
    };

    ~VRFrameReader();

    // The folders holding each game's Wine environment (every Steam library's
    // steamapps/compatdata). Cheap; set them whenever they may have changed.
    void set_search_folders(std::vector<std::string> folders);

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
    std::vector<std::string> search_folders_;

    // Liveness: when frames_submitted last changed, in this process's clock.
    std::uint64_t last_submitted_count_ = 0;
    std::uint64_t last_submitted_change_ns_ = 0;
};

// Every published buffer in the environments under `folders`, newest file first.
std::vector<std::string> discover_vr_shared_paths(const std::vector<std::string>& folders);
