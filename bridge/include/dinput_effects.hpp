#pragma once

#include "wheelio_bridge_protocol.hpp"
#include <cstddef>
#include <cstdint>

namespace wheelio_bridge {

constexpr std::int32_t kDirectInputNominalMax = 10000;
constexpr std::uint32_t kDirectInputInfiniteDuration = 0xFFFFFFFFu;
constexpr std::uint32_t kDirectInputDefaultPeriodicPeriodUs = 100000;

enum class DirectInputEffectKind : std::uint8_t {
    unknown,
    constant,
    ramp,
    square,
    sine,
    triangle,
    sawtooth_up,
    sawtooth_down,
    spring,
    damper,
    inertia,
    friction,
};

enum class DirectInputDirectionMode : std::uint8_t {
    polar,
    cartesian,
    spherical,
};

struct DirectInputEnvelope {
    bool enabled = false;
    std::uint32_t attack_level = 0;
    std::uint32_t attack_time = 0;
    std::uint32_t fade_level = 0;
    std::uint32_t fade_time = 0;
};

struct DirectInputCondition {
    std::int32_t offset = 0;
    std::int32_t positive_coefficient = 0;
    std::int32_t negative_coefficient = 0;
    std::uint32_t positive_saturation = 0;
    std::uint32_t negative_saturation = 0;
    std::int32_t deadband = 0;
};

struct DirectInputEffectState {
    bool started = false;
    std::uint32_t iterations = 1;
    std::uint32_t effect_gain = kDirectInputNominalMax;
    std::uint32_t duration = kDirectInputInfiniteDuration;
    std::uint32_t start_delay = 0;
    std::uint32_t axis_count = 1;
    DirectInputDirectionMode direction_mode = DirectInputDirectionMode::polar;
    std::int32_t direction[2] = {0, 0};
    DirectInputEnvelope envelope{};
    std::uint64_t start_time_us = 0;
    std::uint32_t condition_count = 0;
    DirectInputCondition conditions[2]{};
    std::int32_t constant_magnitude = 0;
    std::int32_t ramp_start = 0;
    std::int32_t ramp_end = 0;
    std::uint32_t periodic_magnitude = kDirectInputNominalMax;
    std::int32_t periodic_offset = 0;
    std::uint32_t periodic_phase = 0;
    std::uint32_t periodic_period = kDirectInputDefaultPeriodicPeriodUs;
};

struct DirectInputEffectView {
    DirectInputEffectKind kind = DirectInputEffectKind::unknown;
    DirectInputEffectState* state = nullptr;
};

bool directinput_effect_is_time_varying(DirectInputEffectKind kind);
bool directinput_effect_has_time_varying_force(DirectInputEffectKind kind,
                                               const DirectInputEffectState& state);
bool directinput_effect_needs_runtime_tick(DirectInputEffectKind kind,
                                           const DirectInputEffectState& state,
                                           std::uint64_t now_us);
bool directinput_effect_is_temporally_active(const DirectInputEffectState& state,
                                             std::uint64_t now_us);
bool directinput_effect_has_expired(const DirectInputEffectState& state,
                                    std::uint64_t now_us);
void directinput_refresh_effect_runtime(DirectInputEffectKind kind,
                                        DirectInputEffectState& state,
                                        std::uint64_t now_us);
void directinput_force_stop_effect(DirectInputEffectState& state);
std::int32_t directinput_compute_force(DirectInputEffectKind kind,
                                       const DirectInputEffectState& state,
                                       std::uint32_t device_gain,
                                       std::uint64_t now_us);
void directinput_apply_effect_to_payload(DirectInputEffectKind kind,
                                         const DirectInputEffectState& state,
                                         std::uint32_t device_gain,
                                         WheelStatePayload& payload,
                                         std::uint64_t now_us);
WheelStatePayload build_directinput_payload(DirectInputEffectView* effects,
                                            std::size_t effect_count,
                                            std::uint32_t device_gain,
                                            bool autocenter_enabled,
                                            bool output_disabled,
                                            std::uint64_t now_us);

}  // namespace wheelio_bridge
