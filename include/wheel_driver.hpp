#pragma once

#include "types.hpp"
#include "ffb_tuning.hpp"
#include "wheelio_bridge_protocol.hpp"
#include <cstdint>

// Manufacturer-agnostic boundary between the bridge server and a physical wheel.
// The server speaks only in generic terms (a WheelStatePayload plus user
// tuning); each concrete driver owns all hardware-specific work: force mapping,
// change-detection, and HID report encoding. To support a new manufacturer,
// implement this interface and register it in wheel_registry.
class WheelDriver {
public:
    virtual ~WheelDriver() = default;

    // Open the device and prepare it for force feedback.
    virtual bool initialize() = 0;
    // Optional startup routine (e.g. LED sweep + force self-check).
    virtual bool calibrate() = 0;
    virtual bool is_initialized() const = 0;

    // Apply a full desired FFB state: the driver maps it to its hardware, diffs
    // against the previous state to avoid redundant writes, and honors the user
    // tuning. Returns false if the device stopped responding.
    virtual bool apply_state(const wheelio_bridge::WheelStatePayload& state,
                             const FfbTuning& tuning) = 0;
    // Set the rev/status LEDs (no-op on wheels without them). Returns false if
    // the device stopped responding.
    virtual bool apply_led_pattern(std::uint8_t pattern) = 0;
    // Release all forces and return the wheel to a neutral state.
    virtual void stop_forces() = 0;
    // Brief, user-triggered confirmation pulse (forces + LEDs).
    virtual void self_test() = 0;

    // The underlying HID device (used to reconcile against hotplug enumeration).
    virtual const HidDevice& device() const = 0;
};
