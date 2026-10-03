#pragma once

#include <vector>
#include <memory>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include "constants.hpp"

using device_id_t = std::uint32_t;
using hid_device_t = __IOHIDDevice;
using hid_manager_t = __IOHIDManager;

template<typename T>
using vector = std::vector<T>;

template<typename T>
using unique_ptr = std::unique_ptr<T>;

template<typename T>
using shared_ptr = std::shared_ptr<T>;

namespace detail {
// IOHIDManagerCopyDevices hands back devices owned by the (transient) CFSet, so
// we CFRetain a device to keep it alive for the lifetime of the HidDevice copies
// that reference it, and CFRelease it once the last copy is destroyed.
inline std::shared_ptr<__IOHIDDevice> retain_hid_device(hid_device_t* device) {
    if (device == nullptr) {
        return nullptr;
    }
    CFRetain(device);
    return std::shared_ptr<__IOHIDDevice>(device, [](hid_device_t* handle) {
        if (handle != nullptr) {
            CFRelease(handle);
        }
    });
}
}  // namespace detail

struct HidDevice {
    device_id_t vendor_id = 0;
    device_id_t product_id = 0;
    device_id_t device_id = 0;
    std::shared_ptr<__IOHIDDevice> hid_device;  // retained handle; see detail::retain_hid_device

    HidDevice() = default;
    HidDevice(device_id_t vid, device_id_t pid, device_id_t did, hid_device_t* device)
        : vendor_id(vid), product_id(pid), device_id(did),
          hid_device(detail::retain_hid_device(device)) {}

    hid_device_t* raw_device() const noexcept { return hid_device.get(); }

    // Max output-report size this interface advertises. The force-feedback
    // interface declares output reports (>0); a pure-input interface reports 0,
    // so this lets us skip opening interfaces we'd never send FFB to (leaving
    // them free for the game to claim).
    std::size_t max_output_report_size() const noexcept {
        if (!hid_device) {
            return 0;
        }
        CFTypeRef value = IOHIDDeviceGetProperty(hid_device.get(), CFSTR(kIOHIDMaxOutputReportSizeKey));
        if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
            long size = 0;
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &size);
            return size > 0 ? static_cast<std::size_t>(size) : 0;
        }
        return 0;
    }

    bool is_valid() const noexcept { return hid_device != nullptr; }
    const WheelProfile* profile() const noexcept { return find_wheel_profile(vendor_id, product_id); }
    bool is_known_wheel() const noexcept { return profile() != nullptr; }
    bool supports_force_feedback() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return wheel_profile && wheel_profile->force_feedback_supported;
    }
    bool supports_leds() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return wheel_profile && wheel_profile->leds_supported;
    }
    bool uses_startup_calibration() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return !wheel_profile || wheel_profile->startup_calibration_enabled;
    }
    bool is_experimental_profile() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return wheel_profile && wheel_profile->experimental;
    }
    std::uint32_t output_report_id() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return wheel_profile ? wheel_profile->output_report_id : 0;
    }
    std::size_t output_report_length() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return wheel_profile ? wheel_profile->output_report_length : COMMAND_MAX_LENGTH;
    }
    const char* display_name() const noexcept {
        const WheelProfile* wheel_profile = profile();
        return wheel_profile ? wheel_profile->name : "Unknown Logitech device";
    }
};

struct Command {
    std::uint8_t data[COMMAND_MAX_LENGTH] = {0};
    
    Command() = default;
    explicit Command(std::initializer_list<std::uint8_t> init);
    
    std::uint8_t& operator[](std::size_t index) noexcept { return data[index]; }
    const std::uint8_t& operator[](std::size_t index) const noexcept { return data[index]; }
    
    const std::uint8_t* raw() const noexcept { return data; }
    std::size_t size() const noexcept { return COMMAND_MAX_LENGTH; }
};
