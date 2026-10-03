#include "ffb_mapping.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wheelio_bridge {

int map_constant_magnitude_to_level(std::int16_t signed_magnitude, double min_force) {
    constexpr int kNominalForceMax = 10000;
    constexpr int kLevelMax = 127;
    // Below this the game means "nothing"; it also avoids buzzing around zero.
    constexpr int kInputDeadzone = 2;

    const int magnitude = std::abs(static_cast<int>(signed_magnitude));
    if (magnitude <= kInputDeadzone) {
        return 0;
    }

    const double normalized = std::min(1.0, static_cast<double>(magnitude) / kNominalForceMax);
    const double floor_level = std::max(0.0, std::min(1.0, min_force)) * kLevelMax;
    int level = static_cast<int>(std::lround(floor_level + (kLevelMax - floor_level) * normalized));
    level = std::min(kLevelMax, std::max(1, level));
    return signed_magnitude < 0 ? -level : level;
}

int apply_constant_slew_limiter(int target_level, bool have_last_level, int last_level, int max_step) {
    if (!have_last_level) {
        return target_level;
    }

    if (max_step >= 254) {
        return target_level;
    }

    // When optional smoothing is enabled, suppress tiny near-zero oscillation
    // that would otherwise buzz while ramping through zero.
    constexpr int kMicroFlipGate = 8;
    if (target_level != 0 && last_level != 0 &&
        (target_level * last_level) < 0 &&
        std::abs(target_level) <= kMicroFlipGate &&
        std::abs(last_level) <= kMicroFlipGate) {
        return 0;
    }

    // Cap the change per update. A large max_step (smoothing off) lets any force
    // change pass through instantly; a small one ramps it for a smoother feel.
    int limited = target_level;
    if (limited > last_level + max_step) {
        limited = last_level + max_step;
    } else if (limited < last_level - max_step) {
        limited = last_level - max_step;
    }

    if (std::abs(limited) <= 1) {
        limited = 0;
    }

    return limited;
}

bool wheel_state_has_effects(const WheelStatePayload& payload) {
    return payload.autocenter_enabled || payload.custom_spring_enabled ||
           payload.damper_enabled || payload.constant_force_enabled;
}

bool wheel_state_equal(const WheelStatePayload& lhs, const WheelStatePayload& rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

}  // namespace wheelio_bridge
