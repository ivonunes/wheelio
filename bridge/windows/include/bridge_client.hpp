#pragma once

#include "desired_wheel_state.hpp"
#include "wheelio_bridge_protocol.hpp"
#include <winsock2.h>
#include <windows.h>

// BridgeClient performs all network I/O on a dedicated worker thread. The game
// thread only deposits the latest desired state into a small mailbox (guarded
// by a CRITICAL_SECTION + CONDITION_VARIABLE) and returns immediately, so a
// missing or slow mac app can never stall the game's input/physics/render
// thread. Newer state overwrites older (coalescing), stop_all takes precedence,
// and the worker owns the socket exclusively (connect / hello / send / reconnect
// backoff all happen there). The worker keeps the last state until it lands:
// a failed send is retried, and when Wheelio drops the connection the state is
// delivered again on reconnect.
class BridgeClient {
public:
    using LogFn = void (*)(const char* message);

    BridgeClient() = default;

    // Where connection events are written. Events are logged on change only
    // (reached, lost, refused), so a missing Mac app logs once, not per retry.
    void set_logger(LogFn log) { log_ = log; }

    void initialize();
    void shutdown();
    void request_shutdown();

    bool send_hello(const char* client_name, std::uint32_t process_id);
    bool send_state(const wheelio_bridge::WheelStatePayload& state);
    bool send_stop_all();

private:
    static DWORD WINAPI worker_thread_entry(LPVOID parameter);
    void run_worker();
    bool ensure_connected();
    bool ensure_hello_sent();
    bool send_message(wheelio_bridge::MessageType type, const void* payload, std::uint32_t payload_size);
    void disconnect();
    void logf(const char* format, ...);
    void note_delivery(bool delivered, const char* what);
    bool peer_closed();

    // Worker-owned socket state (touched only by the worker thread).
    SOCKET socket_ = INVALID_SOCKET;
    bool hello_sent_ = false;
    ULONGLONG next_connect_attempt_us_ = 0;
    LogFn log_ = nullptr;
    // Worker-owned diagnostics: whether the last connect attempt failed (so
    // only the first failure is logged) and updates lost while disconnected.
    bool connect_failure_logged_ = false;
    unsigned long failed_deliveries_ = 0;
    wheelio_bridge::DesiredWheelState desired_;

    // Handoff mailbox shared between the game thread(s) and the worker thread.
    CRITICAL_SECTION lock_;
    CONDITION_VARIABLE wake_;
    wheelio_bridge::WheelStatePayload pending_state_{};
    bool has_pending_state_ = false;
    bool pending_stop_all_ = false;
    bool stop_worker_ = false;
    char last_client_name_[64] = {};
    std::uint32_t last_process_id_ = 0;

    HANDLE worker_ = nullptr;
    bool initialized_ = false;
};
