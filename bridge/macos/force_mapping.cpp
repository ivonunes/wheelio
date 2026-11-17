#include "force_mapping.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kFull = 10000.0;  // DirectInput's full force, coefficient and saturation

// Below this the game means nothing; it also keeps the minimum force from
// buzzing round zero.
constexpr int kForceDeadzone = 2;

double share(double value) {
    return value / kFull;
}

}  // namespace

WheelForces map_wheel_state(const wheelio_bridge::WheelStatePayload& state, const FfbTuning& tuning) {
    WheelForces forces;

    if (std::abs(static_cast<int>(state.constant_force)) > kForceDeadzone) {
        const double size = std::min(1.0, std::abs(share(state.constant_force)));
        const double floor = std::clamp(tuning.min_force, 0.0, 1.0);
        // Past full scale, the session rounds the force off (soft clipping).
        forces.force = std::copysign(floor + (1.0 - floor) * size, static_cast<double>(state.constant_force)) *
                       tuning.force_gain;
    }

    if (state.spring_enabled) {
        WheelSpring spring;
        spring.centre = share(state.spring_offset);
        spring.deadband = share(state.spring_deadband);
        spring.positive_stiffness = share(state.spring_positive_coefficient) * tuning.spring_gain;
        spring.negative_stiffness = share(state.spring_negative_coefficient) * tuning.spring_gain;
        spring.positive_saturation = std::min(1.0, share(state.spring_positive_saturation));
        spring.negative_saturation = std::min(1.0, share(state.spring_negative_saturation));
        forces.spring = spring;
    }

    forces.damper = std::clamp(share(state.damper) * tuning.damper_gain, 0.0, 1.0);
    forces.friction = std::clamp(share(state.friction) * tuning.damper_gain, 0.0, 1.0);
    return forces;
}

double ForceSmoothing::step(double target, double seconds, double smoothing) {
    const double lag = kMostLag * std::clamp(smoothing, 0.0, 1.0);
    if (lag <= 0.0 || seconds <= 0.0) {
        value_ = target;
        return value_;
    }
    value_ += (target - value_) * (1.0 - std::exp(-seconds / lag));
    return value_;
}
