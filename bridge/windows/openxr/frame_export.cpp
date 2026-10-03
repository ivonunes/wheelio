#include "frame_export.hpp"

#include "runtime_log.hpp"
#include "wheelio_vr_shared.hpp"

#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

// %LOCALAPPDATA%\Wheelio\wheelio_vr_<pid>.bin. The mac app finds this by
// globbing the same relative path inside every CrossOver bottle, so neither
// side needs to be told where the other is.
//
// The name carries the process id because the mac app may still have the
// previous run's file mapped: truncating or reusing it would fault that
// process with SIGBUS. A fresh file leaves the old mapping intact until the
// reader moves on.
bool shared_file_path(char* out, std::size_t out_size) {
    char local_appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_appdata))) {
        return false;
    }
    char directory[MAX_PATH] = {};
    std::snprintf(directory, sizeof(directory), "%s\\Wheelio", local_appdata);
    CreateDirectoryA(directory, nullptr);

    // Clear out files left by earlier runs. Deleting one the mac app still has
    // mapped is safe: the mapping stays alive until it closes.
    char pattern[MAX_PATH] = {};
    std::snprintf(pattern, sizeof(pattern), "%s\\wheelio_vr_*.bin", directory);
    WIN32_FIND_DATAA found{};
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char stale[MAX_PATH] = {};
            std::snprintf(stale, sizeof(stale), "%s\\%s", directory, found.cFileName);
            DeleteFileA(stale);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }

    return std::snprintf(out, out_size, "%s\\wheelio_vr_%lu.bin", directory, GetCurrentProcessId()) > 0;
}

wheelio_vr::PixelFormat shared_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return wheelio_vr::PixelFormat::rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return wheelio_vr::PixelFormat::bgra8;
    default:
        return wheelio_vr::PixelFormat::unknown;
    }
}

// Plain global rather than a function-local static: the thread-safe guard for
// a local static drags libwinpthread into the DLL's imports, and that DLL does
// not exist inside the bottle.
LARGE_INTEGER g_qpc_frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return value;
}();

// Nanoseconds since the Unix epoch, from the Windows system clock. Wine reads
// it from the host's realtime clock, so the mac side can subtract directly.
std::uint64_t wall_now_unix_ns() {
    FILETIME file_time;
    GetSystemTimePreciseAsFileTime(&file_time);
    const std::uint64_t ticks_100ns =
        (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32) | file_time.dwLowDateTime;
    constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ULL;  // 1601 -> 1970
    return (ticks_100ns - kUnixEpochIn100ns) * 100ULL;
}

std::uint64_t qpc_now_ns() {
    const LARGE_INTEGER& frequency = g_qpc_frequency;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const std::int64_t seconds = counter.QuadPart / frequency.QuadPart;
    const std::int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return static_cast<std::uint64_t>(seconds * 1000000000LL + (remainder * 1000000000LL) / frequency.QuadPart);
}

}  // namespace

bool FrameExporter::start(GpuReadback* readback, std::uint32_t eye_width, std::uint32_t eye_height,
                          DXGI_FORMAT format) {
    if (started_ || !readback) {
        return started_;
    }
    if (shared_format(format) == wheelio_vr::PixelFormat::unknown) {
        runtime_logf("frame export: unsupported swapchain format %d", static_cast<int>(format));
        return false;
    }
    if (eye_width == 0 || eye_height == 0 || eye_width > wheelio_vr::kMaxEyeWidth ||
        eye_height > wheelio_vr::kMaxEyeHeight) {
        runtime_logf("frame export: refusing eye size %ux%u", eye_width, eye_height);
        return false;
    }

    readback_ = readback;
    eye_width_ = eye_width;
    eye_height_ = eye_height;
    eye_stride_ = eye_width * 4;
    eye_size_ = eye_stride_ * eye_height;

    if (!readback_->start(kPendingCount, eye_width, eye_height, format)) {
        stop();
        return false;
    }

    if (!create_shared_file(eye_width, eye_height, format)) {
        stop();
        return false;
    }

    started_ = true;
    runtime_logf("frame export: publishing %ux%u per eye (%.1f MB per frame)", eye_width, eye_height,
                 (2.0 * eye_size_) / (1024.0 * 1024.0));
    return true;
}

bool FrameExporter::create_shared_file(std::uint32_t eye_width, std::uint32_t eye_height, DXGI_FORMAT format) {
    char path[MAX_PATH] = {};
    if (!shared_file_path(path, sizeof(path))) {
        runtime_log("frame export: could not resolve the shared file path");
        return false;
    }

    const std::uint64_t total = wheelio_vr::total_size(eye_size_);
    std::snprintf(file_path_, sizeof(file_path_), "%s", path);

    file_ = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        runtime_logf("frame export: CreateFile(%s) failed %lu", path, GetLastError());
        return false;
    }

    mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READWRITE, static_cast<DWORD>(total >> 32),
                                  static_cast<DWORD>(total & 0xFFFFFFFF), nullptr);
    if (!mapping_) {
        runtime_logf("frame export: CreateFileMapping failed %lu", GetLastError());
        return false;
    }

    view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(total));
    if (!view_) {
        runtime_logf("frame export: MapViewOfFile failed %lu", GetLastError());
        return false;
    }

    auto* header = static_cast<wheelio_vr::SharedHeader*>(view_);
    std::memset(view_, 0, sizeof(wheelio_vr::SharedHeader));
    header->slot_count = wheelio_vr::kSlotCount;
    header->format = static_cast<std::uint32_t>(shared_format(format));
    header->eye_width = eye_width;
    header->eye_height = eye_height;
    header->eye_stride_bytes = eye_stride_;
    header->eye_size_bytes = eye_size_;
    header->slot_stride_bytes = wheelio_vr::slot_stride(eye_size_);
    header->first_slot_offset = wheelio_vr::kFirstSlotOffset;
    header->total_size_bytes = total;
    header->latest_slot.store(-1, std::memory_order_relaxed);
    header->frames_published.store(0, std::memory_order_relaxed);
    header->frames_dropped.store(0, std::memory_order_relaxed);
    header->reader_heartbeat.store(0, std::memory_order_relaxed);
    header->frames_submitted.store(0, std::memory_order_relaxed);
    std::memset(static_cast<std::uint8_t*>(view_) + wheelio_vr::kHostStateOffset, 0, sizeof(wheelio_vr::HostState));
    header->version = wheelio_vr::kVersion;
    // Magic last: the reader treats it as the signal that the rest is valid.
    std::atomic_thread_fence(std::memory_order_release);
    header->magic = wheelio_vr::kMagic;

    runtime_logf("frame export: shared file %s (%llu MB)", path,
                 static_cast<unsigned long long>(total / (1024 * 1024)));
    return true;
}

void FrameExporter::stop() {
    if (view_) {
        static_cast<wheelio_vr::SharedHeader*>(view_)->magic = 0;
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (file_) {
        CloseHandle(file_);
        file_ = nullptr;
        // A clean shutdown leaves nothing behind; a crash leaves the file for
        // the next launch to clear. The mac side keeps its own mapping alive.
        DeleteFileA(file_path_);
        file_path_[0] = '\0';
    }
    for (Pending& pending : pending_) {
        pending.in_flight = false;
    }
    if (readback_) {
        readback_->stop();
        readback_ = nullptr;
    }
    started_ = false;
}

bool FrameExporter::reader_present(std::uint64_t now_ns) {
    if (!view_) {
        return false;
    }
    const auto* header = static_cast<const wheelio_vr::SharedHeader*>(view_);
    const std::uint64_t heartbeat = header->reader_heartbeat.load(std::memory_order_acquire);
    if (heartbeat != last_heartbeat_) {
        last_heartbeat_ = heartbeat;
        last_heartbeat_change_ns_ = now_ns;
    }
    return last_heartbeat_change_ns_ != 0 && (now_ns - last_heartbeat_change_ns_) < wheelio_vr::kReaderTimeoutNs;
}

void FrameExporter::note_frame_submitted() {
    if (view_) {
        static_cast<wheelio_vr::SharedHeader*>(view_)->frames_submitted.fetch_add(1, std::memory_order_relaxed);
    }
}

bool FrameExporter::read_host_state(wheelio_vr::HostState& out) const {
    if (!view_) {
        return false;
    }
    const auto* state = reinterpret_cast<const wheelio_vr::HostState*>(static_cast<const std::uint8_t*>(view_) +
                                                                       wheelio_vr::kHostStateOffset);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = state->sequence.load(std::memory_order_acquire);
        if (before == 0 || (before % 2) != 0) {
            if (before == 0) {
                return false;  // never written
            }
            continue;
        }
        // Byte copy of a struct holding an atomic: the seqlock, not the atomic,
        // is what makes this read consistent.
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(state), sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (state->sequence.load(std::memory_order_acquire) == before) {
            return true;
        }
    }
    return false;
}

void FrameExporter::submit_frame(std::uint32_t frame_id, std::uint64_t predicted_display_time_ns,
                                 std::uint64_t submitted_at_unix_ns, const ViewInfo views[2]) {
    if (!started_) {
        return;
    }

    // Publish every in-flight capture whose GPU copy has landed, oldest first.
    // A copy usually lands within one frame; when it has not, it waits another
    // frame. The one whose staging texture is needed now is waited for rather
    // than dropped: under a deep GPU queue (in-game load) copies can lag by
    // more frames than we have slots, and dropping would then drop every
    // frame, freezing the headset while the monitor keeps rendering. Waiting
    // throttles the game's submit thread to the GPU instead, as any VR
    // compositor would.
    for (std::uint32_t age = kPendingCount - 1; age >= 1; --age) {
        Pending& candidate = pending_[(next_pending_ + kPendingCount - age) % kPendingCount];
        if (!candidate.in_flight) {
            continue;
        }
        if (publish(candidate, false)) {
            candidate.in_flight = false;
        } else {
            break;  // not ready: anything newer will not be either
        }
    }

    Pending& slot = pending_[next_pending_];
    next_pending_ = (next_pending_ + 1) % kPendingCount;
    if (slot.in_flight) {
        const std::uint64_t started = qpc_now_ns();
        if (!publish(slot, true)) {
            static_cast<wheelio_vr::SharedHeader*>(view_)->frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        ++waited_;
        wait_total_us_ += (qpc_now_ns() - started) / 1000;
        slot.in_flight = false;
    }

    if (++published_ % 1000 == 0) {
        runtime_logf("frame export: %llu frames, waited for the GPU %llu times (%.2f ms each), dropped %llu",
                     static_cast<unsigned long long>(published_), static_cast<unsigned long long>(waited_),
                     waited_ ? static_cast<double>(wait_total_us_) / 1000.0 / static_cast<double>(waited_) : 0.0,
                     static_cast<unsigned long long>(
                         static_cast<wheelio_vr::SharedHeader*>(view_)->frames_dropped.load(std::memory_order_relaxed)));
    }

    const EyeSource sources[2] = {views[0].source, views[1].source};
    if (!sources[0].valid() || !sources[1].valid() ||
        !readback_->copy(static_cast<std::uint32_t>(&slot - pending_), sources)) {
        return;
    }

    slot.in_flight = true;
    slot.frame_id = frame_id;
    slot.predicted_display_time_ns = predicted_display_time_ns;
    slot.submitted_at_unix_ns = submitted_at_unix_ns;
    slot.views[0] = views[0];
    slot.views[1] = views[1];
}

bool FrameExporter::publish(Pending& pending, bool wait) {
    auto* header = static_cast<wheelio_vr::SharedHeader*>(view_);
    auto* base = static_cast<std::uint8_t*>(view_);
    const std::uint32_t slot_index = next_slot_;
    auto* slot = reinterpret_cast<wheelio_vr::SlotHeader*>(base + wheelio_vr::kFirstSlotOffset +
                                                           slot_index * header->slot_stride_bytes);
    auto* pixels = reinterpret_cast<std::uint8_t*>(slot + 1);

    const std::uint64_t map_started_ns = qpc_now_ns();
    const std::uint32_t pending_index = static_cast<std::uint32_t>(&pending - pending_);
    MappedEye mapped[2];
    if (!readback_->map(pending_index, wait, mapped)) {
        return false;
    }

    {
        const std::uint64_t mapped_ns = qpc_now_ns();

        // Mark the slot as being written, copy, then mark it stable again.
        const std::uint32_t sequence = slot->sequence.load(std::memory_order_relaxed);
        slot->sequence.store(sequence + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);

        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            std::uint8_t* destination = pixels + eye * static_cast<std::size_t>(eye_size_);
            const std::uint8_t* source = mapped[eye].data;
            if (mapped[eye].row_pitch == eye_stride_) {
                std::memcpy(destination, source, eye_size_);
            } else {
                for (std::uint32_t row = 0; row < eye_height_; ++row) {
                    std::memcpy(destination + row * static_cast<std::size_t>(eye_stride_),
                                source + row * static_cast<std::size_t>(mapped[eye].row_pitch), eye_stride_);
                }
            }
        }
        readback_->unmap(pending_index);

        const std::uint64_t copied_ns = qpc_now_ns();
        slot->frame_id = pending.frame_id;
        slot->predicted_display_time_ns = pending.predicted_display_time_ns;
        slot->submitted_at_unix_ns = pending.submitted_at_unix_ns;
        slot->published_at_unix_ns = wall_now_unix_ns();
        slot->readback_us = static_cast<std::uint32_t>((mapped_ns - map_started_ns) / 1000);
        slot->copy_us = static_cast<std::uint32_t>((copied_ns - mapped_ns) / 1000);
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            wheelio_vr::ViewPose& pose = slot->views[eye];
            pose.orientation_x = pending.views[eye].orientation[0];
            pose.orientation_y = pending.views[eye].orientation[1];
            pose.orientation_z = pending.views[eye].orientation[2];
            pose.orientation_w = pending.views[eye].orientation[3];
            pose.position_x = pending.views[eye].position[0];
            pose.position_y = pending.views[eye].position[1];
            pose.position_z = pending.views[eye].position[2];
            pose.fov_left = pending.views[eye].fov[0];
            pose.fov_right = pending.views[eye].fov[1];
            pose.fov_up = pending.views[eye].fov[2];
            pose.fov_down = pending.views[eye].fov[3];
        }
        slot->tracking_timestamp_ns = pending.views[0].tracking_timestamp_ns;

        std::atomic_thread_fence(std::memory_order_release);
        slot->sequence.store(sequence + 2, std::memory_order_release);
    }

    header->latest_slot.store(static_cast<std::int32_t>(slot_index), std::memory_order_release);
    header->frames_published.fetch_add(1, std::memory_order_relaxed);
    next_slot_ = (next_slot_ + 1) % wheelio_vr::kSlotCount;
    return true;
}
