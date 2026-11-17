#pragma once

// From the game's force feedback (WheelStatePayload, in DirectInput's units)
// to what Purr's WheelSession plays, with the player's settings applied.

#include "wheel_player.h"
#include "wheelio_bridge_protocol.hpp"

#include <optional>

// The player's settings. A gain of 1 leaves the game's force as it is.
struct FfbTuning {
    double force_gain = 1.0;
    double spring_gain = 1.0;
    // Scales the damper and friction alike.
    double damper_gain = 1.0;
    // 0 is off (most responsive), 1 eases the force the most (ForceSmoothing).
    double smoothing = 0.0;
    // The share of full force any non-zero force starts from, so light road
    // detail moves the wheel past its own dead zone. 0 is off.
    double min_force = 0.0;
};

struct WheelForces {
    double force = 0.0;  // -1 to 1, positive turning the wheel left
    std::optional<WheelSpring> spring;
    double damper = 0.0;    // 0 to 1
    double friction = 0.0;  // 0 to 1
};

WheelForces map_wheel_state(const wheelio_bridge::WheelStatePayload& state, const FfbTuning& tuning);

// The Smoothing setting: eases the force towards what the game asks, with a
// lag of up to kMostLag at full smoothing.
class ForceSmoothing {
public:
    static constexpr double kMostLag = 0.08;  // seconds

    // The force after `seconds` more of `target`.
    double step(double target, double seconds, double smoothing);
    void reset() { value_ = 0.0; }

private:
    double value_ = 0.0;
};
