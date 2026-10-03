#include "dinput_effects.hpp"
#include <algorithm>
#include <cmath>

namespace wheelio_bridge {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;

std::int32_t abs_i32(std::int32_t value) {
    return value < 0 ? -value : value;
}

std::int32_t clamp_i32(std::int32_t value, std::int32_t minimum, std::int32_t maximum) {
    return std::max(minimum, std::min(maximum, value));
}

std::uint32_t clamp_u32(std::uint32_t value, std::uint32_t minimum, std::uint32_t maximum) {
    return std::max(minimum, std::min(maximum, value));
}

std::uint32_t max_u32(std::uint32_t a, std::uint32_t b) {
    return a > b ? a : b;
}

std::uint8_t max_u8(std::uint8_t a, std::uint8_t b) {
    return a > b ? a : b;
}

std::uint8_t scale_byte(std::int32_t value, std::int32_t source_max = kDirectInputNominalMax) {
    const std::int32_t clamped = clamp_i32(value, 0, source_max);
    return static_cast<std::uint8_t>((clamped * 255) / source_max);
}

std::uint8_t scale_nibble(std::int32_t value, std::int32_t source_max = kDirectInputNominalMax) {
    const std::int32_t clamped = clamp_i32(value, 0, source_max);
    return static_cast<std::uint8_t>((clamped * 15) / source_max);
}

std::uint8_t scale_condition_deadband(std::int32_t deadband, std::int32_t offset, bool positive_side) {
    const std::int32_t clamped_offset = clamp_i32(offset, -kDirectInputNominalMax, kDirectInputNominalMax);
    const std::int32_t shifted = positive_side ? deadband + clamped_offset : deadband - clamped_offset;
    return scale_nibble(shifted);
}

std::uint32_t apply_unsigned_gain(std::uint32_t value, std::uint32_t gain) {
    const std::uint32_t clamped_gain = clamp_u32(gain, 0, kDirectInputNominalMax);
    const std::uint64_t scaled = (static_cast<std::uint64_t>(value) * clamped_gain) /
                                 static_cast<std::uint64_t>(kDirectInputNominalMax);
    return static_cast<std::uint32_t>(scaled);
}

std::int32_t apply_signed_gain(std::int32_t value, std::uint32_t gain) {
    const std::uint32_t clamped_gain = clamp_u32(gain, 0, kDirectInputNominalMax);
    const std::int64_t scaled = (static_cast<std::int64_t>(value) * clamped_gain) /
                                static_cast<std::int64_t>(kDirectInputNominalMax);
    return clamp_i32(static_cast<std::int32_t>(scaled), -kDirectInputNominalMax, kDirectInputNominalMax);
}

std::int32_t apply_combined_gain(std::int32_t value, std::uint32_t effect_gain, std::uint32_t device_gain) {
    return apply_signed_gain(apply_signed_gain(value, effect_gain), device_gain);
}

std::uint32_t apply_combined_gain_unsigned(std::uint32_t value,
                                           std::uint32_t effect_gain,
                                           std::uint32_t device_gain) {
    return apply_unsigned_gain(apply_unsigned_gain(value, effect_gain), device_gain);
}

double normalize_phase01(double phase) {
    double result = std::fmod(phase, 1.0);
    if (result < 0.0) {
        result += 1.0;
    }
    return result;
}

double periodic_wave_sample(DirectInputEffectKind kind, double phase01) {
    const double normalized = normalize_phase01(phase01);
    switch (kind) {
        case DirectInputEffectKind::square:
            return normalized < 0.5 ? 1.0 : -1.0;
        case DirectInputEffectKind::triangle:
            return 4.0 * std::abs(normalized - 0.5) - 1.0;
        case DirectInputEffectKind::sawtooth_up:
            return (2.0 * normalized) - 1.0;
        case DirectInputEffectKind::sawtooth_down:
            return 1.0 - (2.0 * normalized);
        case DirectInputEffectKind::sine:
        default:
            return std::sin(normalized * kTwoPi);
    }
}

float direction_multiplier(const DirectInputEffectState& state) {
    if (state.direction_mode == DirectInputDirectionMode::cartesian) {
        if (state.direction[0] == 0) {
            return 1.0f;
        }
        return std::max(-1.0f, std::min(1.0f, static_cast<float>(state.direction[0]) /
                                                static_cast<float>(kDirectInputNominalMax)));
    }

    if (state.axis_count <= 1) {
        return 1.0f;
    }

    const double angle = (static_cast<double>(state.direction[0]) * kTwoPi) / 36000.0;
    if (state.direction_mode == DirectInputDirectionMode::spherical) {
        return static_cast<float>(std::cos(angle));
    }

    // DirectInput polar angles are measured from (0, -1); projected onto the
    // steering axis, 9000 is positive and 27000 is negative.
    return static_cast<float>(std::sin(angle));
}

float envelope_multiplier(const DirectInputEffectState& state,
                          std::uint64_t active_elapsed,
                          std::uint64_t total_duration) {
    if (!state.envelope.enabled) {
        return 1.0f;
    }

    const float attack_level = static_cast<float>(
        clamp_u32(state.envelope.attack_level, 0, kDirectInputNominalMax)) /
        static_cast<float>(kDirectInputNominalMax);
    const float fade_level = static_cast<float>(
        clamp_u32(state.envelope.fade_level, 0, kDirectInputNominalMax)) /
        static_cast<float>(kDirectInputNominalMax);

    if (state.envelope.attack_time > 0 && active_elapsed < state.envelope.attack_time) {
        const float attack_t = static_cast<float>(active_elapsed) /
                               static_cast<float>(state.envelope.attack_time);
        return attack_level + (1.0f - attack_level) * attack_t;
    }

    if (state.duration != kDirectInputInfiniteDuration &&
        state.envelope.fade_time > 0 &&
        total_duration > 0 &&
        active_elapsed < total_duration) {
        const std::uint64_t fade_start =
            total_duration > state.envelope.fade_time ? total_duration - state.envelope.fade_time : 0ULL;
        if (active_elapsed >= fade_start) {
            const std::uint64_t fade_elapsed = active_elapsed - fade_start;
            const float fade_t = static_cast<float>(fade_elapsed) /
                                 static_cast<float>(state.envelope.fade_time);
            return 1.0f + (fade_level - 1.0f) * fade_t;
        }
    }

    return 1.0f;
}

}  // namespace

bool directinput_effect_is_time_varying(DirectInputEffectKind kind) {
    return kind == DirectInputEffectKind::ramp ||
           kind == DirectInputEffectKind::sine ||
           kind == DirectInputEffectKind::square ||
           kind == DirectInputEffectKind::triangle ||
           kind == DirectInputEffectKind::sawtooth_up ||
           kind == DirectInputEffectKind::sawtooth_down;
}

bool directinput_effect_has_time_varying_force(DirectInputEffectKind kind,
                                               const DirectInputEffectState& state) {
    return state.started && (directinput_effect_is_time_varying(kind) || state.envelope.enabled);
}

bool directinput_effect_needs_runtime_tick(DirectInputEffectKind kind,
                                           const DirectInputEffectState& state,
                                           std::uint64_t) {
    if (!state.started) {
        return false;
    }

    if (directinput_effect_has_time_varying_force(kind, state)) {
        return true;
    }

    if (state.start_delay > 0) {
        return true;
    }

    return state.duration != kDirectInputInfiniteDuration &&
           state.iterations != kDirectInputInfiniteDuration &&
           state.duration != 0;
}

bool directinput_effect_is_temporally_active(const DirectInputEffectState& state,
                                             std::uint64_t now_us) {
    if (!state.started) {
        return false;
    }

    if (now_us <= state.start_time_us) {
        return state.start_delay == 0;
    }

    const std::uint64_t elapsed = now_us - state.start_time_us;
    if (elapsed < state.start_delay) {
        return false;
    }

    if (state.duration == kDirectInputInfiniteDuration ||
        state.iterations == kDirectInputInfiniteDuration ||
        state.duration == 0) {
        return true;
    }

    const std::uint64_t total_duration =
        static_cast<std::uint64_t>(state.duration) * static_cast<std::uint64_t>(state.iterations);
    return (elapsed - state.start_delay) < total_duration;
}

bool directinput_effect_has_expired(const DirectInputEffectState& state,
                                    std::uint64_t now_us) {
    if (!state.started ||
        state.duration == kDirectInputInfiniteDuration ||
        state.iterations == kDirectInputInfiniteDuration ||
        state.duration == 0 ||
        now_us <= state.start_time_us) {
        return false;
    }

    const std::uint64_t elapsed = now_us - state.start_time_us;
    if (elapsed < state.start_delay) {
        return false;
    }

    const std::uint64_t total_duration =
        static_cast<std::uint64_t>(state.duration) * static_cast<std::uint64_t>(state.iterations);
    return (elapsed - state.start_delay) >= total_duration;
}

void directinput_refresh_effect_runtime(DirectInputEffectKind,
                                        DirectInputEffectState& state,
                                        std::uint64_t now_us) {
    if (directinput_effect_has_expired(state, now_us)) {
        state.started = false;
        state.iterations = 1;
    }
}

void directinput_force_stop_effect(DirectInputEffectState& state) {
    state.started = false;
    state.iterations = 1;
}

std::int32_t directinput_compute_force(DirectInputEffectKind kind,
                                       const DirectInputEffectState& state,
                                       std::uint32_t device_gain,
                                       std::uint64_t now_us) {
    if (!directinput_effect_is_temporally_active(state, now_us)) {
        return 0;
    }

    const std::uint64_t elapsed = now_us > state.start_time_us ? now_us - state.start_time_us : 0ULL;
    const std::uint64_t active_elapsed = elapsed > state.start_delay ? elapsed - state.start_delay : 0ULL;
    const bool finite_cycle = state.duration != 0 && state.duration != kDirectInputInfiniteDuration;
    const std::uint64_t cycle_duration = finite_cycle ? state.duration : 0ULL;
    const std::uint64_t cycle_elapsed = finite_cycle ? active_elapsed % cycle_duration : active_elapsed;

    std::int32_t raw_force = 0;

    switch (kind) {
        case DirectInputEffectKind::constant:
            raw_force = state.constant_magnitude;
            break;
        case DirectInputEffectKind::ramp: {
            if (state.duration == 0 || state.duration == kDirectInputInfiniteDuration) {
                raw_force = state.ramp_end;
            } else {
                const std::uint32_t ramp_cycle_duration = state.duration == 0 ? 1 : state.duration;
                const std::uint32_t ramp_cycle_elapsed = static_cast<std::uint32_t>(active_elapsed % ramp_cycle_duration);
                const std::int32_t delta = state.ramp_end - state.ramp_start;
                raw_force = state.ramp_start +
                    static_cast<std::int32_t>((static_cast<std::int64_t>(delta) * ramp_cycle_elapsed) /
                                              static_cast<std::int64_t>(ramp_cycle_duration));
            }
            break;
        }
        case DirectInputEffectKind::sine:
        case DirectInputEffectKind::square:
        case DirectInputEffectKind::triangle:
        case DirectInputEffectKind::sawtooth_up:
        case DirectInputEffectKind::sawtooth_down: {
            const std::uint32_t period =
                state.periodic_period == 0 ? kDirectInputDefaultPeriodicPeriodUs : state.periodic_period;
            const double phase_offset = static_cast<double>(state.periodic_phase) / 36000.0;
            const double phase = static_cast<double>(active_elapsed % period) /
                                 static_cast<double>(period) +
                                 phase_offset;
            const double wave = periodic_wave_sample(kind, phase);
            raw_force = static_cast<std::int32_t>(
                state.periodic_offset + static_cast<std::int32_t>(
                    static_cast<double>(state.periodic_magnitude) * wave));
            break;
        }
        default:
            return 0;
    }

    const float shaped = static_cast<float>(raw_force) *
                         envelope_multiplier(state, cycle_elapsed, cycle_duration) *
                         direction_multiplier(state);
    const std::int32_t directed_force =
        clamp_i32(static_cast<std::int32_t>(shaped), -kDirectInputNominalMax, kDirectInputNominalMax);
    return apply_combined_gain(directed_force, state.effect_gain, device_gain);
}

void directinput_apply_effect_to_payload(DirectInputEffectKind kind,
                                         const DirectInputEffectState& state,
                                         std::uint32_t device_gain,
                                         WheelStatePayload& payload,
                                         std::uint64_t now_us) {
    if (!directinput_effect_is_temporally_active(state, now_us)) {
        return;
    }

    if (kind == DirectInputEffectKind::spring) {
        payload.custom_spring_enabled = 1;
        const std::uint32_t positive_sat = apply_combined_gain_unsigned(
            state.conditions[0].positive_saturation, state.effect_gain, device_gain);
        const std::uint32_t negative_sat = apply_combined_gain_unsigned(
            state.conditions[1].negative_saturation, state.effect_gain, device_gain);
        const std::int32_t positive_coeff = apply_combined_gain(
            abs_i32(state.conditions[0].positive_coefficient), state.effect_gain, device_gain);
        const std::int32_t negative_coeff = apply_combined_gain(
            abs_i32(state.conditions[1].negative_coefficient), state.effect_gain, device_gain);

        payload.spring_k1 = max_u8(payload.spring_k1, scale_nibble(positive_coeff));
        payload.spring_k2 = max_u8(payload.spring_k2, scale_nibble(negative_coeff));
        payload.spring_sat1 = max_u8(payload.spring_sat1, scale_nibble(static_cast<std::int32_t>(positive_sat)));
        payload.spring_sat2 = max_u8(payload.spring_sat2, scale_nibble(static_cast<std::int32_t>(negative_sat)));
        payload.spring_deadband_left =
            max_u8(payload.spring_deadband_left,
                   scale_condition_deadband(state.conditions[0].deadband,
                                            state.conditions[0].offset,
                                            false));
        payload.spring_deadband_right =
            max_u8(payload.spring_deadband_right,
                   scale_condition_deadband(state.conditions[1].deadband,
                                            state.conditions[1].offset,
                                            true));
        payload.spring_clip = max_u8(
            payload.spring_clip,
            scale_byte(static_cast<std::int32_t>(max_u32(positive_sat, negative_sat))));
        return;
    }

    if (kind == DirectInputEffectKind::damper ||
        kind == DirectInputEffectKind::friction ||
        kind == DirectInputEffectKind::inertia) {
        payload.damper_enabled = 1;
        payload.damper_force_positive = max_u8(
            payload.damper_force_positive,
            scale_byte(apply_combined_gain(abs_i32(state.conditions[0].positive_coefficient),
                                           state.effect_gain,
                                           device_gain)));
        payload.damper_force_negative = max_u8(
            payload.damper_force_negative,
            scale_byte(apply_combined_gain(abs_i32(state.conditions[1].negative_coefficient),
                                           state.effect_gain,
                                           device_gain)));
        payload.damper_saturation_positive = max_u8(
            payload.damper_saturation_positive,
            scale_byte(static_cast<std::int32_t>(apply_combined_gain_unsigned(
                state.conditions[0].positive_saturation, state.effect_gain, device_gain))));
        payload.damper_saturation_negative = max_u8(
            payload.damper_saturation_negative,
            scale_byte(static_cast<std::int32_t>(apply_combined_gain_unsigned(
                state.conditions[1].negative_saturation, state.effect_gain, device_gain))));
        return;
    }

    const std::int32_t force = directinput_compute_force(kind, state, device_gain, now_us);
    if (force != 0) {
        payload.constant_force_enabled = 1;
        payload.constant_force_magnitude = static_cast<std::int16_t>(
            clamp_i32(static_cast<std::int32_t>(payload.constant_force_magnitude) + force,
                      -kDirectInputNominalMax,
                      kDirectInputNominalMax));
    }
}

WheelStatePayload build_directinput_payload(DirectInputEffectView* effects,
                                            std::size_t effect_count,
                                            std::uint32_t device_gain,
                                            bool autocenter_enabled,
                                            bool output_disabled,
                                            std::uint64_t now_us) {
    WheelStatePayload payload{};

    for (std::size_t i = 0; effects && i < effect_count; ++i) {
        if (!effects[i].state) {
            continue;
        }
        directinput_refresh_effect_runtime(effects[i].kind, *effects[i].state, now_us);
        directinput_apply_effect_to_payload(effects[i].kind, *effects[i].state, device_gain, payload, now_us);
    }

    // DirectInput's autocenter applies while no effect is playing. Stacking it
    // under the game's own road forces would fight them.
    if (autocenter_enabled && !payload.custom_spring_enabled && !payload.damper_enabled &&
        !payload.constant_force_enabled) {
        constexpr std::uint32_t kAutocenterFallbackNominal = 3200;
        payload.autocenter_enabled = 1;
        payload.autocenter_force = max_u8(
            payload.autocenter_force,
            scale_byte(static_cast<std::int32_t>(apply_unsigned_gain(kAutocenterFallbackNominal, device_gain))));
        payload.autocenter_slope = max_u8(payload.autocenter_slope, 4);
    }

    if (output_disabled) {
        payload = WheelStatePayload{};
    }

    return payload;
}

}  // namespace wheelio_bridge
