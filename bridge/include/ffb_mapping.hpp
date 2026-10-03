#pragma once

#include "wheelio_bridge_protocol.hpp"
#include <cstdint>

namespace wheelio_bridge {

// Maps a DirectInput constant force (-10000..10000) to the wheel's signed level
// (-127..127) linearly. `min_force` (0..1) is the fraction of full output any
// non-zero force starts from, so small forces clear the mechanical dead zone.
int map_constant_magnitude_to_level(std::int16_t signed_magnitude, double min_force = 0.0);
// max_step is the largest constant-force change allowed per update: large = no
// smoothing (instant), small = gradual ramp (smoother). Driven by the user's
// Smoothing setting.
int apply_constant_slew_limiter(int target_level, bool have_last_level, int last_level, int max_step);

bool wheel_state_has_effects(const WheelStatePayload& payload);
bool wheel_state_equal(const WheelStatePayload& lhs, const WheelStatePayload& rhs);

}  // namespace wheelio_bridge
