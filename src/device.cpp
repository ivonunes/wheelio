#include "device.hpp"
#include "constants.hpp"
#include "wheel_registry.hpp"
#include "utilities.hpp"
#include <algorithm>
#include <vector>
#include <IOKit/hid/IOHIDKeys.h>

DeviceManager::DeviceManager() : hid_manager_(nullptr) {
    if (!initialize_hid_manager()) {
        Logger::error("Failed to initialize HID manager");
    }
}

DeviceManager::~DeviceManager() {
    cleanup_hid_manager();
}

bool DeviceManager::initialize_hid_manager() {
    hid_manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDManagerOptionNone);
    if (!hid_manager_) {
        Logger::error("IOHIDManagerCreate failed");
        return false;
    }
    
    // Match every supported manufacturer's vendor ID (one matching dict each),
    // so adding a manufacturer in the registry automatically widens enumeration.
    const std::vector<std::uint32_t> vendor_ids = wheel_registry::supported_vendor_ids();
    CFMutableArrayRef matching_array =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

    if (matching_array) {
        for (std::uint32_t vendor : vendor_ids) {
            CFMutableDictionaryRef matching_dict = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (!matching_dict) {
                continue;
            }
            int vendor_id = static_cast<int>(vendor);
            CFNumberRef vendor_id_ref = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor_id);
            CFDictionarySetValue(matching_dict, CFSTR(kIOHIDVendorIDKey), vendor_id_ref);
            CFRelease(vendor_id_ref);
            CFArrayAppendValue(matching_array, matching_dict);
            CFRelease(matching_dict);
        }

        IOHIDManagerSetDeviceMatchingMultiple(hid_manager_, matching_array);
        CFRelease(matching_array);
    } else {
        IOHIDManagerSetDeviceMatching(hid_manager_, nullptr);
    }

    // Hotplug detection: deliver attach/removal callbacks on the main run loop.
    // The callbacks only signal a change (they do no HID work), so sharing the
    // manager with device enumeration on the server thread is safe.
    IOHIDManagerRegisterDeviceMatchingCallback(hid_manager_, &DeviceManager::handle_device_matched, this);
    IOHIDManagerRegisterDeviceRemovalCallback(hid_manager_, &DeviceManager::handle_device_removed, this);
    IOHIDManagerScheduleWithRunLoop(hid_manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    IOReturn result = IOHIDManagerOpen(hid_manager_, kIOHIDOptionsTypeNone);

    if (!ErrorHandler::check_io_result("IOHIDManagerOpen", result)) {
        cleanup_hid_manager();
        return false;
    }

    return true;
}

void DeviceManager::cleanup_hid_manager() {
    if (hid_manager_) {
        Logger::debug("Cleaning up HID manager");

        IOHIDManagerUnscheduleFromRunLoop(hid_manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

        IOReturn result = IOHIDManagerClose(hid_manager_, kIOHIDManagerOptionNone);
        if (result != kIOReturnSuccess) {
            Logger::warning("Failed to close HID manager: " + std::to_string(result));
        }

        CFRelease(hid_manager_);
        hid_manager_ = nullptr;

        Logger::debug("HID manager cleanup complete");
    }
}

void DeviceManager::set_device_change_handler(std::function<void()> handler) {
    device_change_handler_ = std::move(handler);
}

void DeviceManager::notify_device_change() {
    if (device_change_handler_) {
        device_change_handler_();
    }
}

void DeviceManager::handle_device_matched(void* context, IOReturn, void*, IOHIDDeviceRef) {
    if (auto* self = static_cast<DeviceManager*>(context)) {
        self->notify_device_change();
    }
}

void DeviceManager::handle_device_removed(void* context, IOReturn, void*, IOHIDDeviceRef) {
    if (auto* self = static_cast<DeviceManager*>(context)) {
        self->notify_device_change();
    }
}

std::vector<HidDevice> DeviceManager::list_all_devices() {
    std::vector<HidDevice> devices;
    
    if (!hid_manager_) {
        Logger::error("HID manager not initialized");
        return devices;
    }
    
    CFSetRef device_set = IOHIDManagerCopyDevices(hid_manager_);
    if (!device_set) {
        Logger::warning("No HID devices found");
        return devices;
    }
    
    CFIndex count = CFSetGetCount(device_set);
    CFMutableArrayRef device_array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    
    CFSetApplyFunction(device_set, copy_devices_to_array, device_array);
    
    for (CFIndex i = 0; i < count; ++i) {
        hid_device_t* device = static_cast<hid_device_t*>(
            const_cast<void*>(CFArrayGetValueAtIndex(device_array, i))
        );
        
        device_id_t vendor_id = get_device_property_number(device, CFSTR(kIOHIDVendorIDKey));
        device_id_t product_id = get_device_property_number(device, CFSTR(kIOHIDProductIDKey));
        device_id_t device_id = make_device_id(vendor_id, product_id);
        
        devices.emplace_back(vendor_id, product_id, device_id, device);
    }
    
    CFRelease(device_array);
    CFRelease(device_set);
    
    Logger::info("Found " + std::to_string(devices.size()) + " HID devices");
    return devices;
}

std::vector<HidDevice> DeviceManager::find_known_wheels() {
    std::vector<HidDevice> all_devices = list_all_devices();
    std::vector<HidDevice> wheels;
    
    std::copy_if(all_devices.begin(), all_devices.end(), std::back_inserter(wheels),
                    [](const HidDevice& device) {
                        return device.is_known_wheel();
                    });
    
    Logger::info("Found " + std::to_string(wheels.size()) + " known Logitech wheels");
    return wheels;
}

device_id_t DeviceManager::get_device_property_number(hid_device_t* device, CFStringRef property) {
    CFTypeRef data = IOHIDDeviceGetProperty(device, property);
    
    if (data && CFGetTypeID(data) == CFNumberGetTypeID()) {
        device_id_t number;
        CFNumberGetValue(static_cast<CFNumberRef>(data), kCFNumberSInt32Type, &number);
        return number;
    }
    
    return 0;
}

void DeviceManager::copy_devices_to_array(const void* value, void* context) {
    CFArrayAppendValue(static_cast<CFMutableArrayRef>(context), value);
}

HidDeviceInterface::HidDeviceInterface(const HidDevice& device) 
    : device_(device), is_open_(false) {
}

HidDeviceInterface::~HidDeviceInterface() {
    if (is_open_) {
        close();
    }
}

bool HidDeviceInterface::open() {
    if (is_open_) {
        return true;
    }
    
    if (!validate_device()) {
        return false;
    }
    
    IOReturn result = IOHIDDeviceOpen(device_.raw_device(), kIOHIDOptionsTypeNone);

    if (ErrorHandler::check_io_result("IOHIDDeviceOpen", result)) {
        is_open_ = true;
        Logger::debug("Opened device " + utils::format_device_id(device_.device_id));
        return true;
    }
    
    return false;
}

bool HidDeviceInterface::close() {
    if (!is_open_) {
        return true;
    }
    
    Logger::debug("Closing device " + utils::format_device_id(device_.device_id));

    // IOHIDDeviceClose is synchronous for our SetReport-only usage; there are
    // no queued input callbacks to drain.
    IOReturn result = IOHIDDeviceClose(device_.raw_device(), 0);
    bool success = ErrorHandler::check_io_result("IOHIDDeviceClose", result);
    
    if (success) {
        is_open_ = false;
        Logger::debug("Closed device " + utils::format_device_id(device_.device_id));
    } else {
        Logger::error("Failed to close device " + utils::format_device_id(device_.device_id));
    }
    
    return success;
}

bool HidDeviceInterface::send_command(const Command& command) {
    if (!is_open_) {
        Logger::error("Cannot send command: device not open");
        return false;
    }

    const std::size_t report_length = std::min(command.size(), device_.output_report_length());
    
    IOReturn result = IOHIDDeviceSetReport(
        device_.raw_device(),
        kIOHIDReportTypeOutput,
        static_cast<CFIndex>(device_.output_report_id()),
        command.raw(),
        report_length
    );
    
    return ErrorHandler::check_io_result("IOHIDDeviceSetReport", result);
}

bool HidDeviceInterface::validate_device() const {
    if (!device_.is_valid()) {
        Logger::error("Invalid device: null HID device pointer");
        return false;
    }
    
    if (!utils::is_valid_device_id(device_.device_id)) {
        Logger::error("Invalid device: zero device ID");
        return false;
    }
    
    return true;
}
