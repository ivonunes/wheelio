#include "logitech_driver.hpp"
#include "command.hpp"
#include "constants.hpp"
#include "ffb_command_plan.hpp"
#include "utilities.hpp"
#include <unistd.h>
#include <vector>

LogitechWheelDriver::LogitechWheelDriver(const HidDevice& device)
    : device_(device), device_interface_(std::make_unique<HidDeviceInterface>(device)) {
    if (!validate_device()) {
        Logger::error("Invalid device provided to LogitechWheelDriver");
        return;
    }

    Logger::info("Created LogitechWheelDriver for " + std::string(device_.display_name()) +
                 " " + utils::format_device_id(device_.device_id));
}

LogitechWheelDriver::~LogitechWheelDriver() {
    if (!is_initialized_) {
        return;
    }

    Logger::info("Cleaning up LogitechWheelDriver for device " + utils::format_device_id(device_.device_id));

    if (device_interface_ && device_interface_->is_open()) {
        send_stop_forces();
        disable_autocenter();
        set_led_pattern(LED_PATTERN_OFF);

        // Give the device time to process final commands before closing.
        usleep(100 * 1000);
        device_interface_->close();
    }

    Logger::info("LogitechWheelDriver destroyed for device " + utils::format_device_id(device_.device_id));
}

bool LogitechWheelDriver::initialize() {
    if (is_initialized_) {
        return true;
    }

    if (!validate_device()) {
        return false;
    }

    Logger::info("Initializing wheel driver for " + std::string(device_.display_name()) +
                 " " + utils::format_device_id(device_.device_id));

    // Open the device and keep it open for the lifetime of the driver.
    if (!device_interface_->open()) {
        Logger::error("Failed to open device during initialization");
        return false;
    }

    if (!set_led_pattern(LED_PATTERN_OFF)) {
        Logger::warning("Failed to reset LED pattern during initialization");
    }

    is_initialized_ = true;
    Logger::info("Wheel driver initialized successfully");
    return true;
}

bool LogitechWheelDriver::calibrate() {
    if (!is_initialized_) {
        Logger::error("Cannot calibrate: wheel not initialized");
        return false;
    }

    if (is_calibrated_) {
        return true;
    }

    Logger::info("Starting wheel calibration sequence");

    if (!perform_calibration_sequence()) {
        Logger::error("Calibration sequence failed");
        return false;
    }

    is_calibrated_ = true;
    Logger::info("Wheel calibration completed successfully");
    return true;
}

bool LogitechWheelDriver::perform_calibration_sequence() {
    Logger::debug("Starting LED sweep");
    if (!set_led_pattern(LED_PATTERN_OFF)) return false;

    // Forward sweep
    for (int i = 0; i < 32; ++i) {
        usleep(30 * 1000);
        if (!set_led_pattern(static_cast<std::uint8_t>(i))) {
            Logger::warning("LED pattern failed during forward sweep");
        }
    }

    // Backward sweep
    for (int i = 31; i >= 0; --i) {
        usleep(30 * 1000);
        if (!set_led_pattern(static_cast<std::uint8_t>(i))) {
            Logger::warning("LED pattern failed during backward sweep");
        }
    }

    Logger::debug("Testing force feedback");

    if (!disable_autocenter()) return false;
    // About a quarter of full force (128 is none): the strength this nudge had
    // when constant forces were still written to all four slots and summed.
    if (!set_constant_force(96)) return false;

    usleep(500 * 1000);

    if (!send_stop_forces()) return false;
    if (!set_autocenter_spring(2, 2, 48)) return false;
    if (!enable_autocenter()) return false;

    usleep(500 * 1000);

    return true;
}

bool LogitechWheelDriver::apply_state(const wheelio_bridge::WheelStatePayload& payload,
                                      const FfbTuning& tuning) {
    if (!is_initialized_) {
        return false;
    }

    // Plan the commands purely, then send them. If a write fails, roll the plan
    // state back so the update isn't recorded as applied (it'll retry next time).
    const FfbPlanState before = plan_state_;
    const std::vector<Command> commands =
        plan_wheel_commands(payload, tuning, device_.supports_leds(), plan_state_);
    for (const auto& command : commands) {
        if (!send_command(command)) {
            plan_state_ = before;
            return false;
        }
    }
    return true;
}

bool LogitechWheelDriver::apply_led_pattern(std::uint8_t pattern) {
    if (!is_initialized_) {
        return false;
    }

    if (!set_led_pattern(pattern)) {
        return false;
    }

    if (!plan_state_.have_last_wheel_state) {
        plan_state_.last_wheel_state = wheelio_bridge::WheelStatePayload{};
    }
    plan_state_.last_wheel_state.led_pattern_enabled = 1;
    plan_state_.last_wheel_state.led_pattern = pattern;
    plan_state_.have_last_wheel_state = true;
    return true;
}

void LogitechWheelDriver::stop_forces() {
    if (!is_initialized_) {
        return;
    }

    send_stop_forces();
    disable_autocenter();

    plan_state_.last_constant_force_active = false;
    plan_state_.have_last_constant_level = false;
    plan_state_.last_constant_level = 0;
    plan_state_.have_last_wheel_state = false;
    plan_state_.last_wheel_state = wheelio_bridge::WheelStatePayload{};
}

void LogitechWheelDriver::self_test() {
    if (!is_initialized_) {
        return;
    }

    disable_autocenter();
    set_constant_force(168);  // gentle nudge one way (128 = center)
    usleep(200 * 1000);
    set_constant_force(88);   // nudge the other way
    usleep(200 * 1000);
    send_command(CommandBuilder::create_stop_constant_force());

    for (int pattern = 0; pattern <= 31; pattern += 3) {
        set_led_pattern(static_cast<std::uint8_t>(pattern));
        usleep(35 * 1000);
    }
    set_led_pattern(0);

    // Leave forces stopped and reset change-tracking so the next game packet
    // re-applies cleanly.
    stop_forces();
}

bool LogitechWheelDriver::enable_autocenter() {
    Command command = CommandBuilder::create_enable_autocenter();
    return send_command(command);
}

bool LogitechWheelDriver::disable_autocenter() {
    Command command = CommandBuilder::create_disable_autocenter();
    return send_command(command);
}

bool LogitechWheelDriver::set_autocenter_spring(std::uint8_t k1, std::uint8_t k2, std::uint8_t clip) {
    Command command = CommandBuilder::create_autocenter_spring(k1, k2, clip);
    return send_command(command);
}

bool LogitechWheelDriver::set_custom_spring(std::uint8_t d1, std::uint8_t d2, std::uint8_t k1, std::uint8_t k2,
                                            std::uint8_t s1, std::uint8_t s2, std::uint8_t clip) {
    Command command = CommandBuilder::create_custom_spring(d1, d2, k1, k2, s1, s2, clip);
    return send_command(command);
}

bool LogitechWheelDriver::set_constant_force(std::uint8_t force_level) {
    Command command = CommandBuilder::create_constant_force(force_level);
    return send_command(command);
}

bool LogitechWheelDriver::set_damper(std::uint8_t k1, std::uint8_t k2, std::uint8_t s1, std::uint8_t s2) {
    Command command = CommandBuilder::create_damper(k1, k2, s1, s2);
    return send_command(command);
}

bool LogitechWheelDriver::send_stop_forces() {
    Command command = CommandBuilder::create_stop_forces();
    return send_command(command);
}

bool LogitechWheelDriver::set_led_pattern(std::uint8_t pattern) {
    if (!device_.supports_leds()) {
        return true;
    }

    Command command = CommandBuilder::create_led_pattern(pattern);
    return send_command(command);
}

bool LogitechWheelDriver::send_command(const Command& command) {
    if (!device_interface_->is_open()) {
        Logger::error("Device not open for command");
        return false;
    }

    const bool success = device_interface_->send_command(command);
    if (success) {
        Logger::debug("Command sent successfully");
    } else {
        Logger::error("Failed to send command");
    }
    return success;
}

bool LogitechWheelDriver::validate_device() const {
    if (!device_.is_valid()) {
        Logger::error("Invalid HID device");
        return false;
    }

    const WheelProfile* profile = device_.profile();
    if (!profile) {
        Logger::error("Device is not a known Logitech wheel: " + utils::format_device_id(device_.device_id));
        return false;
    }

    if (!profile->force_feedback_supported) {
        Logger::error("Wheel is detected but force feedback is not supported yet: " +
                      std::string(profile->name) + " " + utils::format_device_id(device_.device_id));
        return false;
    }

    return true;
}
