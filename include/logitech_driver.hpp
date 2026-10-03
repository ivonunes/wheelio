#pragma once

#include "wheel_driver.hpp"
#include "ffb_command_plan.hpp"
#include "types.hpp"
#include "device.hpp"
#include "command.hpp"
#include "wheelio_bridge_protocol.hpp"
#include <memory>

// Logitech G-series driver (the legacy "lg4ff" command protocol shared by the
// G923 / G29 / G920). Translates the generic FFB state into Logitech HID reports.
class LogitechWheelDriver : public WheelDriver {
public:
    explicit LogitechWheelDriver(const HidDevice& device);
    ~LogitechWheelDriver() override;

    LogitechWheelDriver(const LogitechWheelDriver&) = delete;
    LogitechWheelDriver& operator=(const LogitechWheelDriver&) = delete;

    bool initialize() override;
    bool calibrate() override;
    bool is_initialized() const override { return is_initialized_; }

    bool apply_state(const wheelio_bridge::WheelStatePayload& state,
                     const FfbTuning& tuning) override;
    bool apply_led_pattern(std::uint8_t pattern) override;
    void stop_forces() override;
    void self_test() override;

    const HidDevice& device() const override { return device_; }

private:
    // lg4ff command senders (the Logitech-specific HID encoding).
    bool enable_autocenter();
    bool disable_autocenter();
    bool set_autocenter_spring(std::uint8_t k1, std::uint8_t k2, std::uint8_t clip);
    bool set_custom_spring(std::uint8_t d1, std::uint8_t d2, std::uint8_t k1, std::uint8_t k2,
                           std::uint8_t s1, std::uint8_t s2, std::uint8_t clip);
    bool set_constant_force(std::uint8_t force_level);
    bool set_damper(std::uint8_t k1, std::uint8_t k2, std::uint8_t s1, std::uint8_t s2);
    bool send_stop_forces();
    bool set_led_pattern(std::uint8_t pattern);
    bool send_command(const Command& command);

    bool validate_device() const;
    bool perform_calibration_sequence();

    HidDevice device_;
    std::unique_ptr<HidDeviceInterface> device_interface_;
    bool is_initialized_ = false;
    bool is_calibrated_ = false;

    // Force mapping + change-detection state (see plan_wheel_commands).
    FfbPlanState plan_state_{};
};
