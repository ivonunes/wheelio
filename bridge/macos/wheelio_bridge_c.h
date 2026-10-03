// Pure C interface over the C++ BridgeServer engine.
//
// This is the entire boundary between the Swift UI and the C++ engine. It
// exposes only POD types (no std::string / std::thread cross the line), so it
// imports cleanly into Swift via the app's bridging header. The engine itself
// (BridgeServer, DeviceManager, WheelController, the wire protocol shared with
// the Windows dinput8.dll) stays in C++ and is unchanged.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a running bridge engine. Created/destroyed by the functions
// below; never dereferenced from Swift.
typedef struct WheelioBridge WheelioBridge;

// Snapshot of the engine state, mirrors BridgeServer::Status. Fixed-size char
// buffers (not std::string) so the struct is trivially importable into Swift.
typedef struct {
    bool     listening;
    bool     client_connected;
    bool     wheel_connected;
    uint16_t port;
    uint64_t packets_received;
    char     client_name[64];
    char     wheel_name[64];
} WheelioBridgeStatus;

// Create an engine bound to `port` (pass 0 for the default port). Returns NULL
// on allocation failure.
WheelioBridge *wheelio_bridge_create(uint16_t port);

// Start the TCP server / wheel service. Safe to call once after create.
void wheelio_bridge_start(WheelioBridge *bridge);

// Stop the server and release force feedback on the wheel. Idempotent.
void wheelio_bridge_stop(WheelioBridge *bridge);

// Force a wheel reconnect/recalibrate cycle. Blocks while it runs; call off the
// main thread.
void wheelio_bridge_reconnect_wheel(WheelioBridge *bridge);

// App-side force-feedback trims (1.0 = unchanged, clamped to [0, 2]). Cheap;
// safe to call from any thread.
void wheelio_bridge_set_force_gain(WheelioBridge *bridge, double gain);
void wheelio_bridge_set_spring_gain(WheelioBridge *bridge, double gain);
void wheelio_bridge_set_damper_gain(WheelioBridge *bridge, double gain);

// Force smoothing: 0 = off (most responsive), 1 = heavy. Cheap; any thread.
void wheelio_bridge_set_smoothing(WheelioBridge *bridge, double amount);

// Minimum force: fraction of full output (0..0.5) that any non-zero force
// starts from, lifting small forces over the wheel's dead zone. 0 = off.
void wheelio_bridge_set_min_force(WheelioBridge *bridge, double fraction);

// Briefly pulse force + LEDs on the connected wheel so the user can confirm it
// works. Blocks (~0.7s); call off the main thread.
void wheelio_bridge_self_test(WheelioBridge *bridge);

// Copy the current status into `out`. Thread-safe (the engine guards it with a
// mutex), cheap to poll.
void wheelio_bridge_status(WheelioBridge *bridge, WheelioBridgeStatus *out);

// Stop (if needed) and destroy the engine.
void wheelio_bridge_destroy(WheelioBridge *bridge);

// --- VR ------------------------------------------------------------------
//
// The OpenXR runtime running inside the CrossOver bottle keeps a shared buffer
// while a game renders in VR. The app watches it to know when to run the
// streamer. Safe to poll; never triggers any work in the game.

typedef struct {
    bool connected;     // a runtime buffer is mapped
    bool game_running;  // frames were submitted in the last few seconds
} WheelioVRStatus;

// Look for a runtime buffer and refresh the connection. Cheap to poll.
void wheelio_vr_poll(WheelioBridge *bridge);

// Copy the current VR status into `out`.
void wheelio_vr_status(WheelioBridge *bridge, WheelioVRStatus *out);

// --- Input Monitoring (TCC) ---------------------------------------------
//
// Talking to the wheel over HID requires the "Input Monitoring" privacy
// permission. These wrap IOHIDCheckAccess/IOHIDRequestAccess so the UI can
// detect a missing grant and prompt for it. No engine instance required.

typedef enum {
    WheelioInputMonitoringGranted = 0,
    WheelioInputMonitoringDenied  = 1,
    WheelioInputMonitoringUnknown = 2,
} WheelioInputMonitoringState;

// Current Input Monitoring access for this process. Cheap to poll.
WheelioInputMonitoringState wheelio_input_monitoring_status(void);

// Ask the user for Input Monitoring. Only shows the system prompt when status
// is Unknown (never asked); once denied the user must use System Settings.
// Returns true if access is granted.
bool wheelio_input_monitoring_request(void);

#ifdef __cplusplus
}
#endif
