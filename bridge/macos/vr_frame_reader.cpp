#include "vr_frame_reader.hpp"

#include <fcntl.h>
#include <glob.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace {

// Purr's runtime writes to %TEMP%\purr_vr_frames_<pid>.bin inside the Wine
// prefix the game runs in: Proton's layout, <compatdata>/<app id>/pfx. Glob
// over the Windows user name rather than assuming one.
constexpr char kSharedPathPattern[] = "/*/pfx/drive_c/users/*/AppData/Local/Temp/purr_vr_frames_*.bin";

std::uint64_t monotonic_ns() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// A VR game submits every frame; this much silence means it has gone.
constexpr std::uint64_t kGameRunningTimeoutNs = 3ULL * 1000000000ULL;

}  // namespace

std::vector<std::string> discover_vr_shared_paths(const std::vector<std::string>& folders) {
    std::vector<std::pair<time_t, std::string>> found;
    for (const std::string& folder : folders) {
        const std::string pattern = folder + kSharedPathPattern;
        glob_t results{};
        if (glob(pattern.c_str(), 0, nullptr, &results) == 0) {
            for (std::size_t i = 0; i < results.gl_pathc; ++i) {
                struct stat info {};
                if (stat(results.gl_pathv[i], &info) == 0) {
                    found.emplace_back(info.st_mtime, results.gl_pathv[i]);
                }
            }
        }
        globfree(&results);
    }

    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::string> paths;
    paths.reserve(found.size());
    for (auto& entry : found) {
        paths.push_back(std::move(entry.second));
    }
    return paths;
}

VRFrameReader::~VRFrameReader() {
    std::lock_guard<std::mutex> guard(mutex_);
    close_locked();
}

void VRFrameReader::close_locked() {
    if (mapping_) {
        munmap(mapping_, mapping_size_);
        mapping_ = nullptr;
        mapping_size_ = 0;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    path_.clear();
    last_submitted_count_ = 0;
    last_submitted_change_ns_ = 0;
}

bool VRFrameReader::open_locked(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat info {};
    if (fstat(fd, &info) != 0 || static_cast<std::size_t>(info.st_size) < sizeof(vr_shared::SharedHeader)) {
        close(fd);
        return false;
    }

    // Only the header page is needed here; the frames are the streamer's business.
    void* mapping = mmap(nullptr, sizeof(vr_shared::SharedHeader), PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return false;
    }

    const auto* header = static_cast<const vr_shared::SharedHeader*>(mapping);
    if (header->magic != vr_shared::kMagic || header->version != vr_shared::kVersion) {
        munmap(mapping, sizeof(vr_shared::SharedHeader));
        close(fd);
        return false;
    }

    fd_ = fd;
    mapping_ = mapping;
    mapping_size_ = sizeof(vr_shared::SharedHeader);
    path_ = path;
    return true;
}

void VRFrameReader::set_search_folders(std::vector<std::string> folders) {
    std::lock_guard<std::mutex> guard(mutex_);
    search_folders_ = std::move(folders);
}

void VRFrameReader::poll() {
    std::lock_guard<std::mutex> guard(mutex_);

    const std::vector<std::string> candidates = discover_vr_shared_paths(search_folders_);

    if (mapping_) {
        const auto* header = static_cast<const vr_shared::SharedHeader*>(mapping_);
        // Each game process publishes its own file, so a newer one at the head
        // of the list means the game restarted and this mapping is stale. The
        // cleared magic means it shut down cleanly.
        const bool superseded = candidates.empty() || candidates.front() != path_;
        if (header->magic == vr_shared::kMagic && !superseded) {
            const std::uint64_t submitted = header->frames_submitted.load(std::memory_order_relaxed);
            if (submitted != last_submitted_count_) {
                last_submitted_count_ = submitted;
                last_submitted_change_ns_ = monotonic_ns();
            }
            return;
        }
        close_locked();
    }

    for (const std::string& candidate : candidates) {
        if (open_locked(candidate)) {
            return;
        }
    }
}

VRFrameReader::Status VRFrameReader::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    Status status;
    if (!mapping_) {
        return status;
    }
    status.connected = true;
    status.game_running = last_submitted_change_ns_ != 0 &&
                          monotonic_ns() - last_submitted_change_ns_ < kGameRunningTimeoutNs;
    status.path = path_;
    const auto* header = static_cast<const vr_shared::SharedHeader*>(mapping_);
    status.eye_width = header->eye_width;
    status.eye_height = header->eye_height;
    return status;
}
