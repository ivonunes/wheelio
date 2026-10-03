#include "wheel_registry.hpp"
#include "logitech_driver.hpp"
#include "constants.hpp"
#include <algorithm>

namespace wheel_registry {

std::vector<std::uint32_t> supported_vendor_ids() {
    std::vector<std::uint32_t> ids;
    for (const auto& profile : SUPPORTED_WHEEL_PROFILES) {
        if (std::find(ids.begin(), ids.end(), profile.vendor_id) == ids.end()) {
            ids.push_back(profile.vendor_id);
        }
    }
    return ids;
}

std::unique_ptr<WheelDriver> create_driver(const HidDevice& device) {
    const WheelProfile* profile = device.profile();
    if (!profile) {
        return nullptr;
    }

    switch (profile->command_protocol) {
        case WheelCommandProtocol::legacy_lg4ff:
            return std::make_unique<LogitechWheelDriver>(device);
    }

    return nullptr;
}

}  // namespace wheel_registry
