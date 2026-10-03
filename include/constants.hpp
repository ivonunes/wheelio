#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

static constexpr std::uint32_t LOGITECH_VENDOR_ID = 0x046D;
static constexpr std::size_t COMMAND_MAX_LENGTH = 7;
static constexpr std::size_t COMMAND_MAX_COUNT = 4;

// Identifies how a wheel's force-feedback commands are encoded. Each value is
// handled by a corresponding WheelDriver (see wheel_registry::create_driver).
// Add a value here when introducing a new manufacturer's protocol.
enum class WheelCommandProtocol : std::uint8_t {
    legacy_lg4ff,  // Logitech G-series (G923/G29/G920)
};

struct WheelProfile {
    std::uint32_t vendor_id = 0;
    std::uint32_t product_id = 0;
    const char* name = "";
    bool force_feedback_supported = false;
    WheelCommandProtocol command_protocol = WheelCommandProtocol::legacy_lg4ff;
    std::uint32_t output_report_id = 0;
    std::size_t output_report_length = COMMAND_MAX_LENGTH;
    bool leds_supported = true;
    bool startup_calibration_enabled = true;
    bool experimental = false;
};

constexpr std::uint32_t make_device_id(std::uint32_t vendor_id, std::uint32_t product_id) {
    return (product_id << 16) | vendor_id;
}

// Every wheel Wheelio recognizes, across all manufacturers. Add new wheels here
// (with their vendor/product IDs and command protocol); the registry derives the
// set of vendor IDs to match and the driver to instantiate from this table.
static constexpr std::array<WheelProfile, 6> SUPPORTED_WHEEL_PROFILES = {{
    {LOGITECH_VENDOR_ID, 0xC266, "Logitech G923 Racing Wheel for PlayStation/PC", true,
     WheelCommandProtocol::legacy_lg4ff, 0, COMMAND_MAX_LENGTH, true, true, false},
    {LOGITECH_VENDOR_ID, 0xC267, "Logitech G923 Racing Wheel for PlayStation/PC", true,
     WheelCommandProtocol::legacy_lg4ff, 0, COMMAND_MAX_LENGTH, true, true, true},
    {LOGITECH_VENDOR_ID, 0xC24F, "Logitech G29 Driving Force Racing Wheel", true,
     WheelCommandProtocol::legacy_lg4ff, 0, COMMAND_MAX_LENGTH, true, true, true},
    {LOGITECH_VENDOR_ID, 0xC262, "Logitech G920 Driving Force Racing Wheel", true,
     WheelCommandProtocol::legacy_lg4ff, 0, COMMAND_MAX_LENGTH, false, true, true},
    {LOGITECH_VENDOR_ID, 0xC26D, "Logitech G923 Racing Wheel for Xbox/PC", true,
     WheelCommandProtocol::legacy_lg4ff, 0, COMMAND_MAX_LENGTH, true, true, true},
    {LOGITECH_VENDOR_ID, 0xC26E, "Logitech G923 Racing Wheel for Xbox/PC", true,
     WheelCommandProtocol::legacy_lg4ff, 0, COMMAND_MAX_LENGTH, true, true, true},
}};

constexpr const WheelProfile* find_wheel_profile(std::uint32_t vendor_id, std::uint32_t product_id) {
    for (const auto& profile : SUPPORTED_WHEEL_PROFILES) {
        if (profile.vendor_id == vendor_id && profile.product_id == product_id) {
            return &profile;
        }
    }
    return nullptr;
}

static constexpr int FORCE_UPDATE_RATE = 8;  // Force feedback update every 8 frames
static constexpr int LED_UPDATE_RATE = 32;   // LED update every 32 frames

static constexpr std::uint8_t LED_PATTERN_OFF = 0x00;
static constexpr std::uint8_t LED_PATTERN_1 = 0x01;
static constexpr std::uint8_t LED_PATTERN_2 = 0x03;
static constexpr std::uint8_t LED_PATTERN_3 = 0x07;
static constexpr std::uint8_t LED_PATTERN_4 = 0x0F;
static constexpr std::uint8_t LED_PATTERN_5 = 0x1F;

static constexpr const char* VERSION = "1.0.0";
