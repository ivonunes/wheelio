#pragma once

#include "force_mapping.hpp"
#include "wheel_session.h"
#include "wheelio_bridge_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// The Mac end of the bridge: takes the game's force feedback from the proxy
// DLL over a local socket and plays it on the wheel through Purr's
// WheelSession, which owns the wheel's drivers.
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

    // Lets the wheel go and takes it again. Returns whether it's open.
    bool reconnect_wheel();
    Status status() const;

    // The player's settings (FfbTuning), clamped to their ranges.
    void set_force_gain(double gain);
    void set_spring_gain(double gain);
    void set_damper_gain(double gain);
    void set_smoothing(double amount);
    void set_min_force(double fraction);
    // A short pulse each way and a sweep of the rev lights. Blocks while it plays.
    void run_self_test();

private:
    using Clock = std::chrono::steady_clock;

    bool open_listen_socket();
    void server_loop();
    void run_client_session(int client_fd);
    // Keeps the wheel open and plays the game's state to it, every kTick.
    void tick();
    void keep_wheel_open(Clock::time_point now);
    // Hands the wheel the game's state: every kTick, and as each new state
    // arrives, so none of the game's steps is lost between ticks.
    void play(Clock::time_point now);

    bool handle_message(int client_fd, const wheelio_bridge::MessageHeader& header);
    bool send_hello_ack(int client_fd);
    void log_ffb_summary_locked(bool force);

    std::uint16_t port_;
    std::atomic<bool> stop_requested_{false};
    std::thread server_thread_;
    int listen_fd_ = -1;

    WheelSession session_;
    // Opening and closing the wheel: the server thread, or a reconnect.
    std::mutex wheel_mutex_;
    // Looking for a wheel to open (server thread only): when to look next,
    // and when the wheel it found was first seen. A wheel just plugged in is
    // given a moment to finish starting up before it's taken.
    Clock::time_point next_look_{};
    std::optional<Clock::time_point> wheel_seen_at_;
    bool looked_ = false;

    // Server thread only.
    Clock::time_point last_tick_{};
    Clock::time_point last_played_{};
    ForceSmoothing smoothing_;
    std::optional<Clock::time_point> self_test_started_;
    std::atomic<bool> self_test_requested_{false};

    // Guards everything below.
    mutable std::mutex mutex_;
    FfbTuning tuning_;
    // The game's last state, while a game is connected and playing.
    std::optional<wheelio_bridge::WheelStatePayload> state_;
    Status status_;
    // Force-feedback traffic since the last system-log summary (once a second
    // while updates arrive).
    struct FfbCounters {
        unsigned received = 0;
        unsigned stops = 0;
        wheelio_bridge::WheelStatePayload last{};
    };
    FfbCounters ffb_counters_;
    Clock::time_point last_ffb_summary_{};
    // Last snapshot returned by status() when mutex_ is busy.
    mutable Status cached_status_;
};
