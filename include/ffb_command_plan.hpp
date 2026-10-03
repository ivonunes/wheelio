#pragma once

#include "command.hpp"
#include "ffb_tuning.hpp"
#include "wheelio_bridge_protocol.hpp"
#include <vector>

// Per-wheel change-detection + force-mapping state for the Logitech (lg4ff)
// command plan. Held by the driver between updates; pure data.
struct FfbPlanState {
    bool have_last_wheel_state = false;
    wheelio_bridge::WheelStatePayload last_wheel_state{};
    bool last_constant_force_active = false;
    bool have_last_constant_level = false;
    int last_constant_level = 0;
    FfbTuning last_tuning{};
    bool have_last_tuning = false;
};

// Plan the lg4ff commands needed to apply `payload` with `tuning`, given the
// prior `state`, advancing `state` to reflect what was applied. `supports_leds`
// gates LED commands. Returns the commands in send order; empty when nothing
// changed. Pure (no I/O) — this is the unit-testable core of the Logitech
// driver: force mapping, gain/smoothing, and redundant-write suppression.
std::vector<Command> plan_wheel_commands(const wheelio_bridge::WheelStatePayload& payload,
                                         const FfbTuning& tuning,
                                         bool supports_leds,
                                         FfbPlanState& state);
