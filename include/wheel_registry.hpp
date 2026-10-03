#pragma once

#include "types.hpp"
#include "wheel_driver.hpp"
#include <cstdint>
#include <memory>
#include <vector>

// The registry maps detected HID devices to the right manufacturer driver.
// Adding support for a new wheel means: add its profile(s) to
// SUPPORTED_WHEEL_PROFILES (constants.hpp) with the appropriate command
// protocol, then handle that protocol in create_driver().
namespace wheel_registry {

// Distinct vendor IDs of all supported manufacturers, for HID device matching.
std::vector<std::uint32_t> supported_vendor_ids();

// Create the driver for a detected device, or nullptr if it isn't supported.
std::unique_ptr<WheelDriver> create_driver(const HidDevice& device);

}  // namespace wheel_registry
