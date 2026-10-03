#pragma once

// User-facing force-feedback tuning, applied by each driver in its own units.
// Gains: 1.0 = unchanged. Smoothing: 0 = off (most responsive), 1 = heavy ramp.
// Minimum force: fraction of full output that any non-zero force starts from,
// to lift small forces over the wheel's mechanical dead zone. 0 = off.
struct FfbTuning {
    double force_gain = 1.0;
    double spring_gain = 1.0;
    double damper_gain = 1.0;
    double smoothing = 0.0;
    double min_force = 0.0;

    bool operator==(const FfbTuning& other) const {
        return force_gain == other.force_gain &&
               spring_gain == other.spring_gain &&
               damper_gain == other.damper_gain &&
               smoothing == other.smoothing &&
               min_force == other.min_force;
    }
    bool operator!=(const FfbTuning& other) const { return !(*this == other); }
};
