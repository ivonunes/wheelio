#pragma once

#include "desired_wheel_state.hpp"
#include "wheelio_bridge_protocol.hpp"
#include "wheel_driver.hpp"
#include "device.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BridgeServer {
public:
    struct Status {
        bool listening = false;
        bool client_connected = false;
        bool wheel_connected = false;
        std::uint16_t port = wheelio_bridge::kDefaultPort;
        std::string client_name;
        std::string wheel_name;
        std::uint64_t packets_received = 0;
    };

    explicit BridgeServer(std::uint16_t port = wheelio_bridge::kDefaultPort);
    ~BridgeServer();

    bool start();
    void stop();

    bool reconnect_wheel();
    Status status() const;

    // App-side force-feedback trims (1.0 = unchanged, clamped to [0, 2]). These
    // scale the game-driven forces before they reach the wheel.
    void set_force_gain(double gain);   // constant force
    void set_spring_gain(double gain);  // centering-spring stiffness
    void set_damper_gain(double gain);  // damper strength
    void set_smoothing(double amount);  // 0 = off (instant), 1 = heavy ramp
    void set_min_force(double fraction);  // 0 = off; fraction of full force small forces start from
    void run_self_test();               // brief force + LED pulse on the wheel.

private:
    enum class WheelConnectResult {
        connected,
        unavailable,
        busy,
    };

    WheelConnectResult ensure_wheel_connected();
    bool complete_wheel_connect_cycle(bool force_reconnect);
    void begin_wheel_operation_locked(const std::string& status_text);
    void finish_wheel_operation_locked(std::vector<std::unique_ptr<WheelDriver>> wheels,
                                       const std::string& status_text);
    void disconnect_wheel_locked();
    void stop_wheel_forces_locked();

    bool open_listen_socket();
    void server_loop();
    void run_client_session(int client_fd);
    void process_pending_device_change();
    void handle_device_change();

    bool handle_message(int client_fd, const wheelio_bridge::MessageHeader& header);
    bool send_hello_ack(int client_fd);
    bool apply_wheel_state_locked(const wheelio_bridge::WheelStatePayload& payload);
    void deliver_desired_state_locked();
    bool apply_led_pattern_locked(std::uint8_t pattern);
    void log_ffb_summary_locked(bool force);

    std::uint16_t port_;
    mutable std::mutex mutex_;
    std::atomic<bool> stop_requested_;
    std::thread server_thread_;

    int listen_fd_;
    // Hotplug is debounced: the callback bumps device_change_seq_; the server
    // thread only reconciles once the sequence has been quiet for an iteration,
    // so USB churn (a flaky device flapping) can't trigger a reconnect storm.
    std::atomic<std::uint64_t> device_change_seq_{0};
    std::uint64_t last_seen_change_seq_ = 0;   // server thread only
    std::uint64_t handled_change_seq_ = 0;     // server thread only
    // When the current run of device changes was first seen; a fresh hot-plug
    // connect waits a grace period from here so the wheel can finish its own
    // power-on init before we open it. Server thread only.
    std::chrono::steady_clock::time_point device_change_observed_at_{};
    double force_gain_ = 1.0;
    double spring_gain_ = 1.0;
    double damper_gain_ = 1.0;
    double smoothing_ = 0.0;  // 0 = off (max responsiveness)
    double min_force_ = 0.0;
    DeviceManager device_manager_;
    // Active drivers (one per opened FFB interface). Each driver owns its own
    // force mapping and change-detection state.
    std::vector<std::unique_ptr<WheelDriver>> wheels_;
    bool wheel_operation_in_progress_ = false;

    Status status_;
    // Force-feedback traffic since the last system-log summary (once a second
    // while updates arrive), so a dead wheel shows whether updates reached the
    // app and what happened to them. Guarded by mutex_.
    struct FfbCounters {
        unsigned received = 0;
        unsigned applied = 0;
        unsigned skipped_busy = 0;
        unsigned failed = 0;
        unsigned stops = 0;
        wheelio_bridge::WheelStatePayload last{};
    };
    FfbCounters ffb_counters_;
    // The game's last force state for this session. A write that fails, an
    // update that arrives while the wheel is calibrating or absent, and a wheel
    // reopened mid-session all leave it pending, and it is applied again as
    // soon as a wheel can take it. Guarded by mutex_.
    wheelio_bridge::DesiredWheelState desired_;
    bool apply_failing_ = false;
    std::chrono::steady_clock::time_point last_ffb_summary_{};
    // Last snapshot returned by status() when mutex_ is busy (e.g. mid HID I/O).
    // Only accessed by the status() caller thread (the UI poll).
    mutable Status cached_status_;
};
