#include "bridge_server.hpp"
#include "wheel_registry.hpp"
#include "utilities.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <os/log.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// Wheelio's bridge diagnostics, read with:
//   log show --last 1h --predicate 'subsystem == "uk.ivonunes.wheelio" && category == "bridge"'
os_log_t bridge_log() {
    static os_log_t log = os_log_create("uk.ivonunes.wheelio", "bridge");
    return log;
}

// After a wheel is hot-plugged, give macOS and the wheel's firmware time to
// finish their own power-on initialization before we open and calibrate it.
// Taking over mid-initialization can leave the wheel in a state other apps
// (e.g. CrossOver) then fail to detect. See process_pending_device_change.
constexpr std::chrono::milliseconds kHotplugConnectGrace{5000};

// A socket timeout (SO_RCVTIMEO/SO_SNDTIMEO) or signal interrupt is not a
// failure — the peer may just be idle between messages. We keep waiting unless
// a shutdown was requested, so the configured timeout only bounds how quickly
// the session notices stop().
bool retry_after_transient_error(const std::atomic<bool>& stop) {
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
    }
    return !stop.load();
}

bool recv_exact(int fd, void* buffer, std::size_t size, const std::atomic<bool>& stop) {
    auto* out = static_cast<std::uint8_t*>(buffer);
    std::size_t received = 0;

    while (received < size) {
        const ssize_t chunk = recv(fd, out + received, size - received, 0);
        if (chunk == 0) {
            return false;
        }
        if (chunk < 0) {
            if (retry_after_transient_error(stop)) {
                continue;
            }
            return false;
        }
        received += static_cast<std::size_t>(chunk);
    }

    return true;
}

bool send_exact(int fd, const void* buffer, std::size_t size, const std::atomic<bool>& stop) {
    const auto* data = static_cast<const std::uint8_t*>(buffer);
    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t chunk = send(fd, data + sent, size - sent, 0);
        if (chunk == 0) {
            return false;
        }
        if (chunk < 0) {
            if (retry_after_transient_error(stop)) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }

    return true;
}

bool discard_exact(int fd, std::size_t size, const std::atomic<bool>& stop) {
    std::array<std::uint8_t, 256> discard{};
    std::size_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk = std::min(remaining, discard.size());
        if (!recv_exact(fd, discard.data(), chunk, stop)) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

void configure_client_socket(int fd) {
    timeval client_timeout{};
    client_timeout.tv_sec = 1;
    client_timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &client_timeout, sizeof(client_timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &client_timeout, sizeof(client_timeout));

    // Disable Nagle's algorithm: force-feedback updates are tiny, latency-critical
    // packets, and Nagle would coalesce/hold them (interacting with delayed-ACK
    // for up to ~40ms of added lag). Send each update immediately.
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

void close_if_open(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

}  // namespace

BridgeServer::BridgeServer(std::uint16_t port)
    : port_(port), stop_requested_(false), listen_fd_(-1), device_manager_() {
    status_.port = port_;
    status_.wheel_name = "Starting wheel service...";

    // Hotplug: the handler runs on the main run loop, so just record that a
    // change happened (no HID work here). The server thread debounces and
    // reconciles once changes settle.
    device_manager_.set_device_change_handler([this]() {
        device_change_seq_.fetch_add(1, std::memory_order_relaxed);
    });
}

BridgeServer::~BridgeServer() {
    stop();
}

bool BridgeServer::open_listen_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return false;
    }

    listen_fd_ = fd;
    return true;
}

bool BridgeServer::start() {
    if (server_thread_.joinable()) {
        return true;
    }

    // Create the listening socket synchronously here, before the worker thread
    // exists, so stop() always observes a valid (or -1) listen_fd_ and there is
    // no race over who creates it.
    if (!open_listen_socket()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.listening = true;
    }

    stop_requested_.store(false);
    server_thread_ = std::thread(&BridgeServer::server_loop, this);
    return true;
}

void BridgeServer::stop() {
    stop_requested_.store(true);

    // Interrupt a blocking select()/accept() on the worker thread. The worker
    // only ever reads listen_fd_; this thread closes it after the join.
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    close_if_open(listen_fd_);

    std::lock_guard<std::mutex> lock(mutex_);
    status_.listening = false;
    status_.client_connected = false;
    status_.client_name.clear();
    stop_wheel_forces_locked();
    disconnect_wheel_locked();
}

bool BridgeServer::reconnect_wheel() {
    return complete_wheel_connect_cycle(true);
}

// The tuning setters just store the value; each driver notices the change on the
// next apply_state and re-applies, so a tweak takes effect on the next packet.
void BridgeServer::set_force_gain(double gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    force_gain_ = std::max(0.0, std::min(2.0, gain));
}

void BridgeServer::set_spring_gain(double gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    spring_gain_ = std::max(0.0, std::min(2.0, gain));
}

void BridgeServer::set_damper_gain(double gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    damper_gain_ = std::max(0.0, std::min(2.0, gain));
}

void BridgeServer::set_smoothing(double amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    smoothing_ = std::max(0.0, std::min(1.0, amount));
}

void BridgeServer::set_min_force(double fraction) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_force_ = std::max(0.0, std::min(0.5, fraction));
}

void BridgeServer::run_self_test() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wheels_.empty() || wheel_operation_in_progress_) {
        return;
    }

    for (auto& wheel : wheels_) {
        if (wheel && wheel->is_initialized()) {
            wheel->self_test();
        }
    }
}

void BridgeServer::handle_device_change() {
    bool have_wheel = false;
    std::vector<hid_device_t*> connected_devices;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_operation_in_progress_) {
            return;
        }
        have_wheel = !wheels_.empty();
        for (const auto& wheel : wheels_) {
            if (wheel) {
                connected_devices.push_back(wheel->device().raw_device());
            }
        }
    }

    // A matching device was attached or removed; reconcile our connection state.
    const auto present = device_manager_.find_known_wheels();
    if (present.empty()) {
        if (have_wheel) {
            std::lock_guard<std::mutex> lock(mutex_);
            disconnect_wheel_locked();
            status_.wheel_name = "Wheel disconnected";
        }
        return;
    }

    if (!have_wheel) {
        complete_wheel_connect_cycle(false);
        return;
    }

    const bool all_connected_interfaces_still_present = std::all_of(
        connected_devices.begin(),
        connected_devices.end(),
        [&present](hid_device_t* connected_device) {
            return connected_device && std::any_of(
                present.begin(),
                present.end(),
                [connected_device](const HidDevice& device) {
                    return device.raw_device() == connected_device;
                });
        });

    if (!all_connected_interfaces_still_present) {
        complete_wheel_connect_cycle(true);
    }
}

void BridgeServer::process_pending_device_change() {
    const std::uint64_t change_seq = device_change_seq_.load(std::memory_order_relaxed);
    if (change_seq == handled_change_seq_) {
        return;
    }

    if (change_seq != last_seen_change_seq_) {
        // New device events are still arriving; note them (and when they began)
        // and wait for the bus to settle before reconciling.
        last_seen_change_seq_ = change_seq;
        device_change_observed_at_ = std::chrono::steady_clock::now();
        return;
    }

    // The change sequence has been quiet for an iteration. When this is a fresh
    // connect (we currently hold no wheel), hold off a little longer: the moment
    // a wheel is plugged in, macOS and the wheel's own firmware run a power-on
    // initialization, and opening/calibrating it mid-initialization can leave the
    // wheel in a state other apps (e.g. CrossOver) then fail to detect. Let that
    // finish first. Removals/reconnects (we already hold a wheel) are handled
    // promptly. Tune kHotplugConnectGrace if a wheel needs more or less time.
    bool connecting = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connecting = wheels_.empty();
    }
    if (connecting &&
        (std::chrono::steady_clock::now() - device_change_observed_at_) < kHotplugConnectGrace) {
        return;  // defer; re-checked on the next loop iteration
    }

    handled_change_seq_ = change_seq;
    handle_device_change();
}

BridgeServer::Status BridgeServer::status() const {
    // Don't block the caller (the UI status poll) if the worker thread is mid
    // wheel update holding mutex_ across HID I/O. Refresh the cache when we can
    // take the lock; otherwise return the last snapshot.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
        cached_status_ = status_;
        return status_;
    }
    return cached_status_;
}

BridgeServer::WheelConnectResult BridgeServer::ensure_wheel_connected() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wheels_.empty()) {
            status_.wheel_connected = true;
            return WheelConnectResult::connected;
        }

        if (wheel_operation_in_progress_) {
            return WheelConnectResult::busy;
        }
    }

    if (complete_wheel_connect_cycle(false)) {
        return WheelConnectResult::connected;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return wheel_operation_in_progress_ ? WheelConnectResult::busy : WheelConnectResult::unavailable;
}

bool BridgeServer::complete_wheel_connect_cycle(bool force_reconnect) {
    std::vector<std::unique_ptr<WheelDriver>> previous_wheels;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!force_reconnect && !wheels_.empty()) {
            status_.wheel_connected = true;
            if (status_.wheel_name.empty()) {
                status_.wheel_name = "Supported wheel connected";
            }
            return true;
        }

        if (wheel_operation_in_progress_) {
            return false;
        }

        begin_wheel_operation_locked(force_reconnect ? "Reconnecting wheel..." : "Connecting to wheel...");
        if (force_reconnect) {
            previous_wheels = std::move(wheels_);
        }
    }

    previous_wheels.clear();

    auto wheels = device_manager_.find_known_wheels();
    if (wheels.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        finish_wheel_operation_locked({}, "No supported wheel detected");
        return false;
    }

    // Open only interfaces that can receive our FFB output reports, so the
    // wheel's input-only interface is left free for the game (CrossOver) to
    // claim. Fall back to every interface if none advertise output, so FFB is
    // never lost to an overly-strict filter.
    std::vector<HidDevice> ffb_interfaces;
    for (const auto& device : wheels) {
        if (device.max_output_report_size() > 0) {
            ffb_interfaces.push_back(device);
        }
    }
    const std::vector<HidDevice>& candidates = ffb_interfaces.empty() ? wheels : ffb_interfaces;

    std::vector<std::unique_ptr<WheelDriver>> initialized_wheels;
    std::vector<std::string> initialized_names;
    std::vector<std::string> unsupported_names;
    bool saw_supported_profile = false;

    for (const auto& device : candidates) {
        const WheelProfile* profile = device.profile();
        if (!profile) {
            continue;
        }

        if (!profile->force_feedback_supported) {
            unsupported_names.emplace_back(profile->name);
            continue;
        }

        saw_supported_profile = true;
        auto controller = wheel_registry::create_driver(device);
        if (!controller || !controller->initialize()) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.wheel_name = "Calibrating " + std::string(profile->name) + "...";
        }

        if (device.uses_startup_calibration() && !controller->calibrate()) {
            continue;
        }
        initialized_names.emplace_back(profile->name);
        initialized_wheels.push_back(std::move(controller));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_wheels.empty()) {
        const std::string base_name = initialized_names.empty() ? "Supported wheel" : initialized_names.front();
        const std::string wheel_name = initialized_wheels.size() > 1
                                           ? base_name + " (" + std::to_string(initialized_wheels.size()) + " interfaces)"
                                           : base_name;
        finish_wheel_operation_locked(std::move(initialized_wheels), wheel_name);
        return true;
    }

    if (!saw_supported_profile && !unsupported_names.empty()) {
        const std::string wheel_name = unsupported_names.size() == 1
                                           ? unsupported_names.front() + " detected (unsupported)"
                                           : "Unsupported wheels detected";
        finish_wheel_operation_locked({}, wheel_name);
        return false;
    }

    finish_wheel_operation_locked({}, "Failed to calibrate supported wheel");
    return false;
}

void BridgeServer::begin_wheel_operation_locked(const std::string& status_text) {
    wheel_operation_in_progress_ = true;
    status_.wheel_connected = false;
    status_.wheel_name = status_text;
}

void BridgeServer::finish_wheel_operation_locked(std::vector<std::unique_ptr<WheelDriver>> wheels,
                                                 const std::string& status_text) {
    wheels_ = std::move(wheels);
    wheel_operation_in_progress_ = false;
    status_.wheel_connected = !wheels_.empty();
    status_.wheel_name = status_text;
    // A freshly opened wheel holds no forces; the game's last state goes back on.
    desired_.redeliver();
}

void BridgeServer::disconnect_wheel_locked() {
    wheels_.clear();
    wheel_operation_in_progress_ = false;
    status_.wheel_connected = false;
    if (status_.wheel_name.empty()) {
        status_.wheel_name = "Disconnected";
    }
}

void BridgeServer::stop_wheel_forces_locked() {
    // Each driver's stop_forces() performs the full release and resets its own
    // change-tracking, so the next game packet re-applies cleanly.
    for (auto& wheel : wheels_) {
        if (wheel && wheel->is_initialized()) {
            wheel->stop_forces();
        }
    }
}

void BridgeServer::server_loop() {
    // This thread receives FFB updates and writes them to the wheel, so give it
    // the highest QoS for prompt scheduling (low force-feedback latency).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // listen_fd_ was created in start() before this thread was spawned, and is
    // closed by stop() after this thread joins, so it is valid for the whole
    // loop and this thread never needs to mutate it.
    complete_wheel_connect_cycle(false);

    while (!stop_requested_.load()) {
        // Debounced hotplug reconciliation also runs inside run_client_session,
        // so wheel attach/removal is handled even while a game keeps a socket open.
        process_pending_device_change();

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;

        const int ready = select(listen_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        configure_client_socket(client_fd);
        os_log(bridge_log(), "game connected");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.client_connected = true;
            status_.client_name = "Connected";
        }

        run_client_session(client_fd);
        close_if_open(client_fd);

        std::lock_guard<std::mutex> lock(mutex_);
        log_ffb_summary_locked(true);
        os_log(bridge_log(), "game disconnected; forces stopped");
        desired_.clear();
        apply_failing_ = false;
        status_.client_connected = false;
        status_.client_name.clear();
        stop_wheel_forces_locked();
    }

    // listen_fd_ is closed by stop() after this thread joins.
}

void BridgeServer::run_client_session(int client_fd) {
    while (!stop_requested_.load()) {
        process_pending_device_change();

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;

        const int ready = select(client_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            deliver_desired_state_locked();
            log_ffb_summary_locked(false);
            continue;
        }

        wheelio_bridge::MessageHeader header{};
        if (!recv_exact(client_fd, &header, sizeof(header), stop_requested_)) {
            os_log(bridge_log(), "connection closed by the game (errno %d)", errno);
            break;
        }

        if (header.magic != wheelio_bridge::kProtocolMagic ||
            header.version != wheelio_bridge::kProtocolVersion) {
            os_log_error(bridge_log(), "protocol mismatch: magic=0x%08x version=%u, expected 0x%08x version %u; "
                         "the game's dinput8.dll is from another Wheelio version",
                         static_cast<unsigned>(header.magic), static_cast<unsigned>(header.version),
                         static_cast<unsigned>(wheelio_bridge::kProtocolMagic),
                         static_cast<unsigned>(wheelio_bridge::kProtocolVersion));
            break;
        }

        if (header.payload_size > wheelio_bridge::kMaxPayloadSize) {
            os_log_error(bridge_log(), "oversized message type=%u size=%u", static_cast<unsigned>(header.type),
                         static_cast<unsigned>(header.payload_size));
            break;
        }

        if (!handle_message(client_fd, header)) {
            os_log_error(bridge_log(), "closing the connection after message type=%u failed",
                         static_cast<unsigned>(header.type));
            break;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        deliver_desired_state_locked();
        log_ffb_summary_locked(false);
    }
}

bool BridgeServer::handle_message(int client_fd, const wheelio_bridge::MessageHeader& header) {
    switch (static_cast<wheelio_bridge::MessageType>(header.type)) {
        case wheelio_bridge::MessageType::hello: {
            if (header.payload_size != sizeof(wheelio_bridge::HelloPayload)) {
                return false;
            }

            wheelio_bridge::HelloPayload payload{};
            if (!recv_exact(client_fd, &payload, sizeof(payload), stop_requested_)) {
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.client_name = payload.client_name;
                char name[sizeof(payload.client_name) + 1] = {};
                std::memcpy(name, payload.client_name, sizeof(payload.client_name));
                os_log(bridge_log(), "hello from %{public}s pid=%u; wheel_connected=%d wheel=%{public}s", name,
                       static_cast<unsigned>(payload.process_id), status_.wheel_connected ? 1 : 0,
                       status_.wheel_name.c_str());
            }

            return send_hello_ack(client_fd);
        }

        case wheelio_bridge::MessageType::apply_wheel_state: {
            if (header.payload_size != sizeof(wheelio_bridge::WheelStatePayload)) {
                return false;
            }

            wheelio_bridge::WheelStatePayload payload{};
            if (!recv_exact(client_fd, &payload, sizeof(payload), stop_requested_)) {
                return false;
            }

            const auto connect_result = ensure_wheel_connected();
            std::lock_guard<std::mutex> lock(mutex_);
            ++ffb_counters_.received;
            ffb_counters_.last = payload;
            desired_.set(payload);
            // The game stays connected whatever happens to the wheel; the
            // state is applied once a wheel can take it.
            if (connect_result == WheelConnectResult::unavailable) {
                if (!apply_failing_) {
                    os_log_error(bridge_log(), "force update with no wheel available (%{public}s); "
                                 "applying it when a wheel connects", status_.wheel_name.c_str());
                    apply_failing_ = true;
                }
                return true;
            }
            if (connect_result == WheelConnectResult::busy) {
                ++ffb_counters_.skipped_busy;
                return true;
            }
            deliver_desired_state_locked();
            return true;
        }

        case wheelio_bridge::MessageType::stop_all: {
            if (header.payload_size != 0 && !discard_exact(client_fd, header.payload_size, stop_requested_)) {
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            desired_.clear();
            stop_wheel_forces_locked();
            ++ffb_counters_.stops;
            ++status_.packets_received;
            return true;
        }

        case wheelio_bridge::MessageType::ping: {
            if (header.payload_size > 0 && !discard_exact(client_fd, header.payload_size, stop_requested_)) {
                return false;
            }
            return true;
        }

        case wheelio_bridge::MessageType::set_led_pattern: {
            if (header.payload_size != sizeof(wheelio_bridge::LedPatternPayload)) {
                return false;
            }

            wheelio_bridge::LedPatternPayload payload{};
            if (!recv_exact(client_fd, &payload, sizeof(payload), stop_requested_)) {
                return false;
            }

            const auto connect_result = ensure_wheel_connected();
            if (connect_result == WheelConnectResult::unavailable) {
                return false;
            }
            if (connect_result == WheelConnectResult::busy) {
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            return apply_led_pattern_locked(payload.pattern);
        }

        default:
            return false;
    }
}

bool BridgeServer::send_hello_ack(int client_fd) {
    wheelio_bridge::HelloAckPayload payload{};
    payload.accepted = 1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        payload.wheel_connected = status_.wheel_connected ? 1 : 0;
        payload.server_port = port_;
        std::strncpy(payload.wheel_name, status_.wheel_name.c_str(), sizeof(payload.wheel_name) - 1);
    }

    wheelio_bridge::MessageHeader header{};
    header.type = static_cast<std::uint16_t>(wheelio_bridge::MessageType::hello_ack);
    header.payload_size = sizeof(payload);

    return send_exact(client_fd, &header, sizeof(header), stop_requested_) &&
            send_exact(client_fd, &payload, sizeof(payload), stop_requested_);
}

bool BridgeServer::apply_wheel_state_locked(const wheelio_bridge::WheelStatePayload& payload) {
    if (wheel_operation_in_progress_) {
        // Calibrating or reconnecting: not applied, so it stays pending.
        ++ffb_counters_.skipped_busy;
        return false;
    }

    if (wheels_.empty()) {
        status_.wheel_connected = false;
        return false;
    }

    // Hand the generic state + user tuning to each driver; the driver owns all
    // manufacturer-specific mapping, change-detection and report encoding.
    const FfbTuning tuning{force_gain_, spring_gain_, damper_gain_, smoothing_, min_force_};
    bool applied_to_any_wheel = false;
    for (auto& wheel : wheels_) {
        if (wheel && wheel->is_initialized() && wheel->apply_state(payload, tuning)) {
            applied_to_any_wheel = true;
        }
    }

    if (!applied_to_any_wheel) {
        ++ffb_counters_.failed;
        if (!apply_failing_) {
            os_log_error(bridge_log(), "no wheel accepted the force update (%zu open, initialised=%d); "
                         "retrying", wheels_.size(),
                         wheels_.front() && wheels_.front()->is_initialized() ? 1 : 0);
            apply_failing_ = true;
        }
        status_.wheel_connected = false;
        return false;
    }

    if (apply_failing_) {
        os_log(bridge_log(), "force updates reach the wheel again");
        apply_failing_ = false;
    }
    ++ffb_counters_.applied;
    ++status_.packets_received;
    return true;
}

// Applies the game's last state if it hasn't reached the wheel yet and a wheel
// can take it; called on each new state and on the session's idle ticks.
void BridgeServer::deliver_desired_state_locked() {
    if (!desired_.needs_delivery() || wheels_.empty() || wheel_operation_in_progress_) {
        return;
    }
    if (apply_wheel_state_locked(desired_.state())) {
        desired_.delivered();
    }
}

// Once a second while force updates arrive (or when forced at disconnect):
// what came in, what reached the wheel and the last state, with the tuning
// that maps it, so the game's force range can be read off a session.
void BridgeServer::log_ffb_summary_locked(bool force) {
    const auto now = std::chrono::steady_clock::now();
    const auto& c = ffb_counters_;
    if (c.received == 0 && c.stops == 0) {
        return;
    }
    if (!force && now - last_ffb_summary_ < std::chrono::seconds(1)) {
        return;
    }
    os_log(bridge_log(),
           "ffb: received=%u applied=%u skipped_busy=%u failed=%u stops=%u | last constant=%d mag=%d "
           "spring=%d k=%u/%u clip=%u damper=%d autocenter=%d | gain=%.2f spring_gain=%.2f "
           "damper_gain=%.2f min_force=%.2f smoothing=%.2f",
           c.received, c.applied, c.skipped_busy, c.failed, c.stops, c.last.constant_force_enabled,
           static_cast<int>(c.last.constant_force_magnitude), c.last.custom_spring_enabled,
           static_cast<unsigned>(c.last.spring_k1), static_cast<unsigned>(c.last.spring_k2),
           static_cast<unsigned>(c.last.spring_clip), c.last.damper_enabled, c.last.autocenter_enabled,
           force_gain_, spring_gain_, damper_gain_, min_force_, smoothing_);
    ffb_counters_ = FfbCounters{};
    last_ffb_summary_ = now;
}

bool BridgeServer::apply_led_pattern_locked(std::uint8_t pattern) {
    if (wheel_operation_in_progress_) {
        return true;
    }

    if (wheels_.empty()) {
        status_.wheel_connected = false;
        return false;
    }

    bool applied = false;
    for (auto& wheel : wheels_) {
        if (wheel && wheel->is_initialized()) {
            applied = wheel->apply_led_pattern(pattern) || applied;
        }
    }

    ++status_.packets_received;
    return applied;
}
