#include "bridge_client.hpp"
#include <windows.h>
#include <ws2tcpip.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr DWORD kSocketTimeoutMs = 750;

bool send_exact(SOCKET socket_handle, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t sent = 0;

    while (sent < size) {
        const int chunk = send(socket_handle, bytes + sent, static_cast<int>(size - sent), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }

    return true;
}

bool recv_exact(SOCKET socket_handle, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    std::size_t received = 0;

    while (received < size) {
        const int chunk = recv(socket_handle, bytes + received, static_cast<int>(size - received), 0);
        if (chunk <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(chunk);
    }

    return true;
}

void copy_c_string(char* destination, std::size_t destination_size, const char* source) {
    if (!destination || destination_size == 0) {
        return;
    }

    if (!source) {
        destination[0] = '\0';
        return;
    }

    const std::size_t source_length = std::strlen(source);
    const std::size_t copy_length = (source_length < (destination_size - 1)) ? source_length : (destination_size - 1);
    std::memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

void configure_socket_timeouts(SOCKET socket_handle) {
    const DWORD timeout = kSocketTimeoutMs;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // Disable Nagle's algorithm: FFB updates are tiny, latency-critical packets
    // that must go out immediately rather than being coalesced/held.
    BOOL nodelay = TRUE;
    setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
}

bool connect_with_timeout(SOCKET socket_handle, const sockaddr* address, int address_size) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
        return connect(socket_handle, address, address_size) == 0;
    }

    int result = connect(socket_handle, address, address_size);
    if (result == 0) {
        nonblocking = 0;
        ioctlsocket(socket_handle, FIONBIO, &nonblocking);
        return true;
    }

    const int connect_error = WSAGetLastError();
    if (connect_error != WSAEWOULDBLOCK && connect_error != WSAEINPROGRESS) {
        nonblocking = 0;
        ioctlsocket(socket_handle, FIONBIO, &nonblocking);
        return false;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(socket_handle, &write_fds);

    fd_set error_fds;
    FD_ZERO(&error_fds);
    FD_SET(socket_handle, &error_fds);

    timeval timeout{};
    timeout.tv_sec = static_cast<long>(kSocketTimeoutMs / 1000);
    timeout.tv_usec = static_cast<long>((kSocketTimeoutMs % 1000) * 1000);

    result = select(0, nullptr, &write_fds, &error_fds, &timeout);

    bool connected = false;
    if (result > 0 && FD_ISSET(socket_handle, &write_fds)) {
        int socket_error = 0;
        int socket_error_size = sizeof(socket_error);
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &socket_error_size) == 0) {
            connected = socket_error == 0;
        }
    }

    nonblocking = 0;
    ioctlsocket(socket_handle, FIONBIO, &nonblocking);
    return connected;
}

// Backoff between failed connect attempts so a missing mac app does not cause
// the worker thread to spin reconnecting (and burning CPU).
constexpr ULONGLONG kReconnectBackoffUs = 1000ULL * 1000ULL;  // 1 second

// Finite wait so the worker can also drive reconnect backoff even when no new
// state arrives.
constexpr DWORD kWorkerWaitMs = 250;

ULONGLONG monotonic_us() {
    return static_cast<ULONGLONG>(GetTickCount64()) * 1000ULL;
}

}  // namespace

void BridgeClient::initialize() {
    if (initialized_) {
        return;
    }

    InitializeCriticalSection(&lock_);
    InitializeConditionVariable(&wake_);
    socket_ = INVALID_SOCKET;
    hello_sent_ = false;
    next_connect_attempt_us_ = 0;
    has_pending_state_ = false;
    pending_stop_all_ = false;
    stop_worker_ = false;
    std::memset(last_client_name_, 0, sizeof(last_client_name_));
    last_process_id_ = 0;

    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        DeleteCriticalSection(&lock_);
        return;
    }

    worker_ = CreateThread(nullptr, 0, &BridgeClient::worker_thread_entry, this, 0, nullptr);
    if (!worker_) {
        WSACleanup();
        DeleteCriticalSection(&lock_);
        return;
    }

    initialized_ = true;
}

void BridgeClient::request_shutdown() {
    // Safe to call under loader lock: only sets a flag and wakes the worker.
    // Never joins.
    if (!initialized_) {
        return;
    }

    EnterCriticalSection(&lock_);
    stop_worker_ = true;
    WakeConditionVariable(&wake_);
    LeaveCriticalSection(&lock_);
}

void BridgeClient::shutdown() {
    // Full teardown. JOINs the worker, so it must NOT run under loader lock.
    if (!initialized_) {
        return;
    }

    EnterCriticalSection(&lock_);
    stop_worker_ = true;
    WakeConditionVariable(&wake_);
    LeaveCriticalSection(&lock_);

    if (worker_) {
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
        worker_ = nullptr;
    }

    disconnect();
    DeleteCriticalSection(&lock_);
    WSACleanup();
    initialized_ = false;
}

bool BridgeClient::send_hello(const char* client_name, std::uint32_t process_id) {
    if (!initialized_) {
        return false;
    }

    EnterCriticalSection(&lock_);
    const char* effective_name = (client_name && client_name[0]) ? client_name : "WheelioProxy";
    copy_c_string(last_client_name_, sizeof(last_client_name_), effective_name);
    last_process_id_ = process_id;
    // Store the identity used by the worker's lazy hello on the next bridge send.
    WakeConditionVariable(&wake_);
    LeaveCriticalSection(&lock_);
    return true;
}

bool BridgeClient::send_state(const wheelio_bridge::WheelStatePayload& state) {
    if (!initialized_) {
        return false;
    }

    EnterCriticalSection(&lock_);
    // Newer state overwrites older: coalesce and drop stale state.
    pending_state_ = state;
    has_pending_state_ = true;
    WakeConditionVariable(&wake_);
    LeaveCriticalSection(&lock_);
    return true;
}

bool BridgeClient::send_stop_all() {
    if (!initialized_) {
        return false;
    }

    EnterCriticalSection(&lock_);
    pending_stop_all_ = true;
    // Drop any pending state so it cannot override the stop.
    has_pending_state_ = false;
    WakeConditionVariable(&wake_);
    LeaveCriticalSection(&lock_);
    return true;
}

DWORD WINAPI BridgeClient::worker_thread_entry(LPVOID parameter) {
    static_cast<BridgeClient*>(parameter)->run_worker();
    return 0;
}

void BridgeClient::run_worker() {
    // This thread delivers FFB updates to the mac app; bump its priority so a
    // send isn't delayed behind ordinary game threads.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    EnterCriticalSection(&lock_);
    for (;;) {
        // Wait until there is work to do. Each timeout (the CS is released while
        // sleeping) retries an undelivered state, paced by the reconnect
        // backoff, and notices Wheelio closing the connection while idle.
        while (!stop_worker_ && !pending_stop_all_ && !has_pending_state_) {
            const BOOL woken = SleepConditionVariableCS(&wake_, &lock_, kWorkerWaitMs);
            if (woken) {
                continue;
            }
            if (socket_ != INVALID_SOCKET && peer_closed()) {
                logf("bridge: Wheelio closed the connection; reconnecting");
                disconnect();
                desired_.redeliver();
            }
            if (desired_.needs_delivery()) {
                break;
            }
        }

        if (stop_worker_) {
            break;
        }

        // stop_all takes precedence over any pending state. If it can't be
        // sent, Wheelio stops the forces itself when the connection goes.
        if (pending_stop_all_) {
            pending_stop_all_ = false;
            desired_.clear();
            LeaveCriticalSection(&lock_);

            bool ok = ensure_connected() && ensure_hello_sent() &&
                      send_message(wheelio_bridge::MessageType::stop_all, nullptr, 0);
            note_delivery(ok, "stop_all");
            if (!ok) {
                disconnect();
            }

            EnterCriticalSection(&lock_);
            continue;
        }

        if (has_pending_state_) {
            desired_.set(pending_state_);
            has_pending_state_ = false;
        }
        LeaveCriticalSection(&lock_);

        if (desired_.needs_delivery()) {
            const wheelio_bridge::WheelStatePayload state = desired_.state();
            bool ok = ensure_connected() && ensure_hello_sent() &&
                      send_message(wheelio_bridge::MessageType::apply_wheel_state, &state, sizeof(state));
            note_delivery(ok, "force update");
            if (ok) {
                desired_.delivered();
            } else {
                disconnect();
            }
        }

        EnterCriticalSection(&lock_);
    }
    LeaveCriticalSection(&lock_);
}

bool BridgeClient::ensure_connected() {
    if (socket_ != INVALID_SOCKET) {
        return true;
    }

    // Respect reconnect backoff so a missing mac app does not spin the CPU.
    const ULONGLONG now = monotonic_us();
    if (next_connect_attempt_us_ != 0 && now < next_connect_attempt_us_) {
        return false;
    }
    next_connect_attempt_us_ = now + kReconnectBackoffUs;

    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        return false;
    }
    configure_socket_timeouts(socket_);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(wheelio_bridge::kDefaultPort);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        disconnect();
        return false;
    }

    if (!connect_with_timeout(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) {
        const int error = WSAGetLastError();
        disconnect();
        if (!connect_failure_logged_) {
            logf("bridge: cannot reach Wheelio on 127.0.0.1:%u (error %d); is the app running? retrying every second",
                 static_cast<unsigned>(wheelio_bridge::kDefaultPort), error);
            connect_failure_logged_ = true;
        }
        return false;
    }

    logf("bridge: connected to Wheelio on 127.0.0.1:%u", static_cast<unsigned>(wheelio_bridge::kDefaultPort));
    connect_failure_logged_ = false;
    return true;
}

bool BridgeClient::ensure_hello_sent() {
    if (hello_sent_) {
        return true;
    }

    wheelio_bridge::HelloPayload hello{};
    char client_name[sizeof(last_client_name_)] = {};
    std::uint32_t process_id = 0;
    EnterCriticalSection(&lock_);
    copy_c_string(client_name, sizeof(client_name), last_client_name_[0] ? last_client_name_ : "WheelioProxy");
    process_id = last_process_id_;
    LeaveCriticalSection(&lock_);

    copy_c_string(hello.client_name, sizeof(hello.client_name), client_name);
    hello.process_id = process_id;

    if (!send_message(wheelio_bridge::MessageType::hello, &hello, sizeof(hello))) {
        logf("bridge: sending hello failed (error %d)", WSAGetLastError());
        return false;
    }

    wheelio_bridge::MessageHeader header{};
    wheelio_bridge::HelloAckPayload ack{};
    if (!recv_exact(socket_, &header, sizeof(header))) {
        // Wheelio closes the connection on a protocol mismatch rather than replying.
        logf("bridge: no reply to hello (error %d); a Wheelio speaking another protocol version closes the connection",
             WSAGetLastError());
        return false;
    }
    if (header.magic != wheelio_bridge::kProtocolMagic || header.version != wheelio_bridge::kProtocolVersion ||
        header.type != static_cast<std::uint16_t>(wheelio_bridge::MessageType::hello_ack) ||
        header.payload_size != sizeof(ack)) {
        logf("bridge: unexpected hello reply magic=0x%08lx version=%u (proxy speaks %u) type=%u size=%lu",
             static_cast<unsigned long>(header.magic), static_cast<unsigned>(header.version),
             static_cast<unsigned>(wheelio_bridge::kProtocolVersion), static_cast<unsigned>(header.type),
             static_cast<unsigned long>(header.payload_size));
        return false;
    }
    if (!recv_exact(socket_, &ack, sizeof(ack))) {
        logf("bridge: hello reply cut short (error %d)", WSAGetLastError());
        return false;
    }
    char wheel_name[sizeof(ack.wheel_name) + 1] = {};
    std::memcpy(wheel_name, ack.wheel_name, sizeof(ack.wheel_name));
    if (!ack.accepted) {
        logf("bridge: Wheelio refused the hello (wheel_connected=%u wheel=\"%s\")",
             static_cast<unsigned>(ack.wheel_connected), wheel_name);
        return false;
    }
    logf("bridge: Wheelio accepted %s pid=%lu; wheel_connected=%u wheel=\"%s\"", client_name,
         static_cast<unsigned long>(process_id), static_cast<unsigned>(ack.wheel_connected), wheel_name);

    hello_sent_ = true;
    return true;
}

bool BridgeClient::send_message(wheelio_bridge::MessageType type, const void* payload, std::uint32_t payload_size) {
    wheelio_bridge::MessageHeader header{};
    header.type = static_cast<std::uint16_t>(type);
    header.payload_size = payload_size;

    if (!send_exact(socket_, &header, sizeof(header))) {
        return false;
    }

    if (payload_size > 0 && payload != nullptr) {
        return send_exact(socket_, payload, payload_size);
    }

    return true;
}

// Logs the first failed delivery and the recovery, with how many attempts
// failed in between; the last state is retried until it lands.
void BridgeClient::note_delivery(bool delivered, const char* what) {
    if (delivered) {
        if (failed_deliveries_ > 0) {
            logf("bridge: delivered after %lu failed attempt(s)", failed_deliveries_);
            failed_deliveries_ = 0;
        }
        return;
    }
    if (failed_deliveries_ == 0) {
        logf("bridge: %s not delivered%s; retrying until Wheelio is reachable", what,
             socket_ != INVALID_SOCKET ? " (send failed, reconnecting)" : "");
    }
    ++failed_deliveries_;
}

// Wheelio never sends anything after the hello reply, so a readable socket
// means it closed the connection (or the connection broke).
bool BridgeClient::peer_closed() {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_, &read_fds);
    timeval immediate{};
    if (select(0, &read_fds, nullptr, nullptr, &immediate) <= 0) {
        return false;
    }
    char byte = 0;
    const int received = recv(socket_, &byte, 1, MSG_PEEK);
    return received <= 0;
}

void BridgeClient::logf(const char* format, ...) {
    if (!log_) {
        return;
    }
    char buffer[512] = {0};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_(buffer);
}

void BridgeClient::disconnect() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    hello_sent_ = false;
}
