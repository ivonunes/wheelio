#include "ffb_command_plan.hpp"
#include "ffb_mapping.hpp"
#include <algorithm>
#include <cmath>

namespace {

// Scale a force-strength field by a user gain, clamped to its valid range.
// A gain of exactly 1.0 passes the game's value through untouched.
std::uint8_t scale_strength(std::uint8_t value, double gain, int max_value) {
    if (gain == 1.0) {
        return value;
    }
    const long scaled = std::lround(static_cast<double>(value) * gain);
    return static_cast<std::uint8_t>(std::max<long>(0, std::min<long>(max_value, scaled)));
}

void append_stop_commands(std::vector<Command>& commands) {
    commands.push_back(CommandBuilder::create_stop_forces());
    commands.push_back(CommandBuilder::create_disable_autocenter());
}

}  // namespace

std::vector<Command> plan_wheel_commands(const wheelio_bridge::WheelStatePayload& payload,
                                         const FfbTuning& tuning,
                                         bool supports_leds,
                                         FfbPlanState& state) {
    std::vector<Command> commands;

    // A tuning change must re-apply even when the game's payload is unchanged.
    if (!state.have_last_tuning || tuning != state.last_tuning) {
        state.have_last_wheel_state = false;
        state.have_last_constant_level = false;
    }
    state.last_tuning = tuning;
    state.have_last_tuning = true;

    const bool has_any_effect = wheelio_bridge::wheel_state_has_effects(payload);

    int desired_constant_level = 0;
    if (payload.constant_force_enabled) {
        desired_constant_level =
            wheelio_bridge::map_constant_magnitude_to_level(payload.constant_force_magnitude, tuning.min_force);
        if (tuning.force_gain != 1.0) {
            const long scaled = std::lround(static_cast<double>(desired_constant_level) * tuning.force_gain);
            desired_constant_level = static_cast<int>(std::max<long>(-127, std::min<long>(127, scaled)));
        }
        // Map the smoothing setting to a per-update step: 0 -> 254 (full swing in
        // one update, no smoothing) up to ~8 (heavy ramp). Squared so the
        // slider's lower range already eases in some smoothing.
        const double responsiveness = (1.0 - tuning.smoothing) * (1.0 - tuning.smoothing);
        const int max_step = static_cast<int>(
            std::max<long>(8, std::min<long>(254, std::lround(254.0 * responsiveness))));
        desired_constant_level = wheelio_bridge::apply_constant_slew_limiter(
            desired_constant_level, state.have_last_constant_level, state.last_constant_level, max_step);
    }

    const bool constant_active = desired_constant_level != 0;
    const bool constant_level_changed = constant_active
        ? (!state.have_last_constant_level || desired_constant_level != state.last_constant_level)
        : state.last_constant_force_active;

    if (!has_any_effect) {
        append_stop_commands(commands);
        state.last_constant_force_active = false;
        state.have_last_constant_level = false;
        state.last_constant_level = 0;
        state.have_last_wheel_state = true;
        state.last_wheel_state = payload;
        return commands;
    }

    if (state.have_last_wheel_state &&
        wheelio_bridge::wheel_state_equal(payload, state.last_wheel_state) &&
        !constant_level_changed) {
        return commands;  // nothing changed
    }

    const bool spring_changed =
        !state.have_last_wheel_state ||
        payload.custom_spring_enabled != state.last_wheel_state.custom_spring_enabled ||
        payload.spring_deadband_left != state.last_wheel_state.spring_deadband_left ||
        payload.spring_deadband_right != state.last_wheel_state.spring_deadband_right ||
        payload.spring_k1 != state.last_wheel_state.spring_k1 ||
        payload.spring_k2 != state.last_wheel_state.spring_k2 ||
        payload.spring_sat1 != state.last_wheel_state.spring_sat1 ||
        payload.spring_sat2 != state.last_wheel_state.spring_sat2 ||
        payload.spring_clip != state.last_wheel_state.spring_clip;
    const bool damper_changed =
        !state.have_last_wheel_state ||
        payload.damper_enabled != state.last_wheel_state.damper_enabled ||
        payload.damper_force_positive != state.last_wheel_state.damper_force_positive ||
        payload.damper_force_negative != state.last_wheel_state.damper_force_negative ||
        payload.damper_saturation_positive != state.last_wheel_state.damper_saturation_positive ||
        payload.damper_saturation_negative != state.last_wheel_state.damper_saturation_negative;
    const bool autocenter_changed =
        !state.have_last_wheel_state ||
        payload.autocenter_enabled != state.last_wheel_state.autocenter_enabled ||
        payload.autocenter_force != state.last_wheel_state.autocenter_force ||
        payload.autocenter_slope != state.last_wheel_state.autocenter_slope;
    const bool constant_command_changed = constant_level_changed;
    const bool led_changed =
        !state.have_last_wheel_state ||
        payload.led_pattern_enabled != state.last_wheel_state.led_pattern_enabled ||
        payload.led_pattern != state.last_wheel_state.led_pattern;

    if (spring_changed) {
        if (payload.custom_spring_enabled) {
            commands.push_back(CommandBuilder::create_custom_spring(
                payload.spring_deadband_left,
                payload.spring_deadband_right,
                scale_strength(payload.spring_k1, tuning.spring_gain, 15),
                scale_strength(payload.spring_k2, tuning.spring_gain, 15),
                payload.spring_sat1,
                payload.spring_sat2,
                payload.spring_clip));
        } else {
            commands.push_back(CommandBuilder::create_stop_spring());
        }
    }

    if (damper_changed) {
        if (payload.damper_enabled) {
            commands.push_back(CommandBuilder::create_damper(
                scale_strength(payload.damper_force_positive, tuning.damper_gain, 255),
                scale_strength(payload.damper_force_negative, tuning.damper_gain, 255),
                payload.damper_saturation_positive,
                payload.damper_saturation_negative));
        } else {
            commands.push_back(CommandBuilder::create_stop_damper());
        }
    }

    if (autocenter_changed) {
        if (payload.autocenter_enabled) {
            commands.push_back(CommandBuilder::create_enable_autocenter());
            commands.push_back(CommandBuilder::create_autocenter_spring(
                payload.autocenter_slope, payload.autocenter_slope, payload.autocenter_force));
        } else {
            commands.push_back(CommandBuilder::create_disable_autocenter());
        }
    }

    if (constant_command_changed) {
        if (constant_active) {
            const auto raw_level = static_cast<std::uint8_t>(
                std::max(0, std::min(255, 128 + desired_constant_level)));
            commands.push_back(CommandBuilder::create_constant_force(raw_level));
        } else if (state.last_constant_force_active) {
            commands.push_back(CommandBuilder::create_stop_constant_force());
        }
    }

    if (led_changed && supports_leds) {
        commands.push_back(CommandBuilder::create_led_pattern(
            payload.led_pattern_enabled ? payload.led_pattern : 0));
    }

    state.last_constant_force_active = constant_active;
    if (constant_active) {
        state.have_last_constant_level = true;
        state.last_constant_level = desired_constant_level;
    } else {
        state.have_last_constant_level = false;
        state.last_constant_level = 0;
    }
    state.have_last_wheel_state = true;
    state.last_wheel_state = payload;
    return commands;
}
