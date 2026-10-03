#include "vr_frame_reader.hpp"

#include <fcntl.h>
#include <glob.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace {

// The runtime writes to %LOCALAPPDATA%\Wheelio\wheelio_vr_<pid>.bin inside
// whichever bottle the game runs in. The Windows user name is chosen by
// CrossOver, so glob over it rather than assuming "crossover".
constexpr char kSharedPathPattern[] =
    "/Library/Application Support/CrossOver/Bottles/*/drive_c/users/*/AppData/Local/Wheelio/wheelio_vr_*.bin";

std::uint64_t monotonic_ns() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// A VR game submits every frame; this much silence means it has gone.
constexpr std::uint64_t kGameRunningTimeoutNs = 3ULL * 1000000000ULL;

}  // namespace

std::vector<std::string> discover_vr_shared_paths() {
    const char* home = getenv("HOME");
    if (!home) {
        return {};
    }
    const std::string pattern = std::string(home) + kSharedPathPattern;

    glob_t results{};
    if (glob(pattern.c_str(), 0, nullptr, &results) != 0) {
        globfree(&results);
        return {};
    }

    std::vector<std::pair<time_t, std::string>> found;
    found.reserve(results.gl_pathc);
    for (std::size_t i = 0; i < results.gl_pathc; ++i) {
        struct stat info {};
        if (stat(results.gl_pathv[i], &info) == 0) {
            found.emplace_back(info.st_mtime, results.gl_pathv[i]);
        }
    }
    globfree(&results);

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
    if (fstat(fd, &info) != 0 || static_cast<std::size_t>(info.st_size) < sizeof(wheelio_vr::SharedHeader)) {
        close(fd);
        return false;
    }

    // Only the header page is needed here; the frames are the streamer's business.
    void* mapping = mmap(nullptr, sizeof(wheelio_vr::SharedHeader), PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return false;
    }

    const auto* header = static_cast<const wheelio_vr::SharedHeader*>(mapping);
    if (header->magic != wheelio_vr::kMagic || header->version != wheelio_vr::kVersion) {
        munmap(mapping, sizeof(wheelio_vr::SharedHeader));
        close(fd);
        return false;
    }

    fd_ = fd;
    mapping_ = mapping;
    mapping_size_ = sizeof(wheelio_vr::SharedHeader);
    path_ = path;
    return true;
}

void VRFrameReader::poll() {
    std::lock_guard<std::mutex> guard(mutex_);

    const std::vector<std::string> candidates = discover_vr_shared_paths();

    if (mapping_) {
        const auto* header = static_cast<const wheelio_vr::SharedHeader*>(mapping_);
        // Each game process publishes its own file, so a newer one at the head
        // of the list means the game restarted and this mapping is stale. The
        // cleared magic means it shut down cleanly.
        const bool superseded = candidates.empty() || candidates.front() != path_;
        if (header->magic == wheelio_vr::kMagic && !superseded) {
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
    return status;
}
