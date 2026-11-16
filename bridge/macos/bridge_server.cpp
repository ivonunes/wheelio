#include "bridge_server.hpp"

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

// After a wheel is plugged in, give macOS and the wheel's firmware time to
// finish their own start before taking it. Taking it mid-start can leave the
// wheel in a state other apps (CrossOver) then fail to detect.
constexpr std::chrono::milliseconds kHotplugConnectGrace{5000};

// How often the game's state is played to the wheel. WheelSession plays it on
// at the wheel's own rate in between, and fades the wheel out if nothing is
// set for a while, so it's set on every tick whether or not it changed.
constexpr std::chrono::milliseconds kTick{10};

// How often to look for a wheel while none is open.
constexpr std::chrono::milliseconds kLookInterval{1000};

// The self-test: a pulse each way, then the rev lights swept once.
constexpr std::chrono::milliseconds kSelfTestPulse{150};
constexpr std::chrono::milliseconds kSelfTestLength{700};
constexpr double kSelfTestForce = 0.3;

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

BridgeServer::BridgeServer(std::uint16_t port) : port_(port) {
    status_.port = port_;
    status_.wheel_name = "Looking for a wheel...";
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
    // exists, so stop() always observes a valid (or -1) listen_fd_.
    if (!open_listen_socket()) {
        return false;
    }

    // Wheels plugged in starting in another mode (a G923 in Xbox mode) are
    // switched to their own as they arrive, on the main run loop.
    WheelOutput::watch();

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

    // Interrupt a blocking select()/accept() on the worker thread.
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    close_if_open(listen_fd_);

    {
        std::lock_guard<std::mutex> wheel(wheel_mutex_);
        session_.close();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    status_.listening = false;
    status_.client_connected = false;
    status_.client_name.clear();
    status_.wheel_connected = false;
    state_.reset();
}

bool BridgeServer::reconnect_wheel() {
    std::lock_guard<std::mutex> wheel(wheel_mutex_);
    session_.close();
    const bool opened = session_.open();
    const std::optional<FoundWheel> found = session_.wheel();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.wheel_connected = opened;
    status_.wheel_name = opened && found ? found->name : "No supported wheel detected";
    return opened;
}

void BridgeServer::set_force_gain(double gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    tuning_.force_gain = std::clamp(gain, 0.0, 2.0);
}

void BridgeServer::set_spring_gain(double gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    tuning_.spring_gain = std::clamp(gain, 0.0, 2.0);
}

void BridgeServer::set_damper_gain(double gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    tuning_.damper_gain = std::clamp(gain, 0.0, 2.0);
}

void BridgeServer::set_smoothing(double amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    tuning_.smoothing = std::clamp(amount, 0.0, 1.0);
}

void BridgeServer::set_min_force(double fraction) {
    std::lock_guard<std::mutex> lock(mutex_);
    tuning_.min_force = std::clamp(fraction, 0.0, 0.5);
}

void BridgeServer::run_self_test() {
    self_test_requested_.store(true);
    std::this_thread::sleep_for(kSelfTestLength);
}

BridgeServer::Status BridgeServer::status() const {
    // Don't block the caller (the UI's status poll) on the server thread.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
        cached_status_ = status_;
        return status_;
    }
    return cached_status_;
}

void BridgeServer::keep_wheel_open(Clock::time_point now) {
    std::unique_lock<std::mutex> wheel(wheel_mutex_, std::try_to_lock);
    if (!wheel.owns_lock() || session_.is_open() || now < next_look_) {
        return;  // a reconnect is under way, or there's nothing to do yet
    }
    next_look_ = now + kLookInterval;

    if (session_.wheel()) {
        // It was open and stopped answering: unplugged.
        session_.close();
        os_log(bridge_log(), "the wheel stopped answering");
        std::lock_guard<std::mutex> lock(mutex_);
        status_.wheel_connected = false;
        status_.wheel_name = "Wheel disconnected";
    }

    const std::optional<FoundWheel> found = session_.find_wheel();
    const bool first_look = !looked_;
    looked_ = true;
    if (!found) {
        wheel_seen_at_.reset();
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.wheel_name != "Wheel disconnected") {
            status_.wheel_name = "No supported wheel detected";
        }
        return;
    }

    // One there as Wheelio starts is taken at once; one plugged in later is
    // given its grace first.
    if (!first_look) {
        if (!wheel_seen_at_) {
            wheel_seen_at_ = now;
        }
        if (now - *wheel_seen_at_ < kHotplugConnectGrace) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.wheel_name = "Connecting to " + found->name + "...";
            return;
        }
    }

    const bool opened = session_.open();
    wheel_seen_at_.reset();
    smoothing_.reset();
    if (opened) {
        os_log(bridge_log(), "opened %{public}s (%{public}s)", found->name.c_str(), found->family.c_str());
    } else {
        os_log_error(bridge_log(), "couldn't open %{public}s; trying again", found->name.c_str());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.wheel_connected = opened;
    status_.wheel_name = opened ? found->name : "Couldn't connect to " + found->name;
}

void BridgeServer::play(Clock::time_point now) {
    if (!session_.is_open()) {
        return;
    }

    const double seconds = last_played_ == Clock::time_point{}
                               ? 0.0
                               : std::chrono::duration<double>(now - last_played_).count();
    last_played_ = now;
    WheelForces forces;
    double smoothing = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_) {
            forces = map_wheel_state(*state_, tuning_);
        }
        smoothing = tuning_.smoothing;
    }
    double force = smoothing_.step(forces.force, seconds, smoothing);
    std::uint32_t leds = 0;

    if (self_test_requested_.exchange(false)) {
        self_test_started_ = now;
    }
    if (self_test_started_) {
        const auto elapsed = now - *self_test_started_;
        if (elapsed < kSelfTestLength) {
            if (elapsed < kSelfTestPulse) {
                force = kSelfTestForce;
            } else if (elapsed < 2 * kSelfTestPulse) {
                force = -kSelfTestForce;
            }
            leds = WheelSession::rev_lights(std::chrono::duration<double>(elapsed) / kSelfTestLength);
        } else {
            self_test_started_.reset();
        }
    }

    session_.set_force(force);
    if (forces.spring) {
        session_.set_spring(*forces.spring);
    } else {
        session_.clear_spring();
    }
    session_.set_damper(forces.damper);
    session_.set_friction(forces.friction);
    session_.set_leds(leds);
}

void BridgeServer::tick() {
    const Clock::time_point now = Clock::now();
    if (last_tick_ != Clock::time_point{} && now - last_tick_ < kTick) {
        return;
    }
    keep_wheel_open(now);
    play(now);
    last_tick_ = now;

    std::lock_guard<std::mutex> lock(mutex_);
    status_.wheel_connected = session_.is_open();
    log_ffb_summary_locked(false);
}

void BridgeServer::server_loop() {
    // This thread plays the game's forces to the wheel, so give it the highest
    // QoS for prompt scheduling.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    while (!stop_requested_.load()) {
        tick();

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(kTick).count();

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
        state_.reset();
        status_.client_connected = false;
        status_.client_name.clear();
    }
}

void BridgeServer::run_client_session(int client_fd) {
    while (!stop_requested_.load()) {
        tick();

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);

        timeval timeout{};
        timeout.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(kTick).count();

        const int ready = select(client_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
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
                char name[sizeof(payload.client_name) + 1] = {};
                std::memcpy(name, payload.client_name, sizeof(payload.client_name));
                status_.client_name = name;
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

            // Played now, and every tick after it until the game changes it,
            // whether or not a wheel is there yet.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = payload;
                ++ffb_counters_.received;
                ffb_counters_.last = payload;
                ++status_.packets_received;
            }
            play(Clock::now());
            return true;
        }

        case wheelio_bridge::MessageType::stop_all: {
            if (header.payload_size != 0 && !discard_exact(client_fd, header.payload_size, stop_requested_)) {
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.reset();
                ++ffb_counters_.stops;
                ++status_.packets_received;
            }
            play(Clock::now());
            return true;
        }

        case wheelio_bridge::MessageType::ping: {
            return header.payload_size == 0 || discard_exact(client_fd, header.payload_size, stop_requested_);
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

// Once a second while force updates arrive (or when forced at disconnect):
// what came in and the last state, with the settings that map it, so the
// game's force range can be read off a session.
void BridgeServer::log_ffb_summary_locked(bool force) {
    const auto now = Clock::now();
    const auto& c = ffb_counters_;
    if (c.received == 0 && c.stops == 0) {
        return;
    }
    if (!force && now - last_ffb_summary_ < std::chrono::seconds(1)) {
        return;
    }
    os_log(bridge_log(),
           "ffb: received=%u stops=%u wheel=%d | last constant=%d spring=%d offset=%d deadband=%u k=%d/%d "
           "sat=%u/%u damper=%u friction=%u | gain=%.2f spring_gain=%.2f damper_gain=%.2f min_force=%.2f "
           "smoothing=%.2f",
           c.received, c.stops, status_.wheel_connected ? 1 : 0, static_cast<int>(c.last.constant_force),
           c.last.spring_enabled, static_cast<int>(c.last.spring_offset),
           static_cast<unsigned>(c.last.spring_deadband), static_cast<int>(c.last.spring_positive_coefficient),
           static_cast<int>(c.last.spring_negative_coefficient),
           static_cast<unsigned>(c.last.spring_positive_saturation),
           static_cast<unsigned>(c.last.spring_negative_saturation), static_cast<unsigned>(c.last.damper),
           static_cast<unsigned>(c.last.friction), tuning_.force_gain, tuning_.spring_gain, tuning_.damper_gain,
           tuning_.min_force, tuning_.smoothing);
    ffb_counters_ = FfbCounters{};
    last_ffb_summary_ = now;
}
