#include "dinput_effects.hpp"
#include "ffb_mapping.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect_true(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void expect_eq(int actual, int expected, const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " expected " << expected << " got " << actual << '\n';
        std::exit(1);
    }
}

void expect_near(int actual, int expected, int tolerance, const std::string& message) {
    const int delta = actual > expected ? actual - expected : expected - actual;
    if (delta > tolerance) {
        std::cerr << "FAIL: " << message << " expected " << expected << " +/- " << tolerance
                  << " got " << actual << '\n';
        std::exit(1);
    }
}

void test_constant_mapping() {
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(0), 0, "zero force maps to zero");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(2), 0, "positive deadzone maps to zero");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(-2), 0, "negative deadzone maps to zero");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(64), 1, "small force maps to the smallest level");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(5000), 64, "mid force maps linearly");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(-5000), -64, "negative mid force maps linearly");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(10000), 127, "nominal force is full level");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(-10000), -127, "negative nominal force is full level");
    // A minimum force lifts every non-zero force onto a floor and scales the rest above it.
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(64, 0.2), 26, "minimum force sets the floor");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(10000, 0.2), 127, "minimum force keeps full scale");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(-5000, 0.2), -76, "minimum force scales above the floor");
    expect_eq(wheelio_bridge::map_constant_magnitude_to_level(0, 0.2), 0, "minimum force leaves zero at zero");
}

void test_slew_limiter() {
    // Smoothing off (large step): changes pass through instantly.
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(80, false, 0, 254), 80, "first level passes through");
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(80, true, 0, 254), 80, "no smoothing: step passes through");
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(-80, true, 80, 254), -80, "no smoothing: reversal passes");
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(5, true, -5, 254), 5, "no smoothing: micro reversal passes");
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(1, true, 0, 254), 1, "no smoothing: tiny output passes");
    // Smoothing on (small step): the change is ramped.
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(80, true, 0, 24), 24, "smoothing limits the step");
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(5, true, -5, 24), 0, "smoothing gates micro sign flip");
    expect_eq(wheelio_bridge::apply_constant_slew_limiter(1, true, 0, 24), 0, "smoothing snaps tiny output to zero");
}

void test_payload_helpers() {
    wheelio_bridge::WheelStatePayload empty{};
    wheelio_bridge::WheelStatePayload with_led{};
    with_led.led_pattern_enabled = 1;
    with_led.led_pattern = 7;

    wheelio_bridge::WheelStatePayload with_force{};
    with_force.constant_force_enabled = 1;
    with_force.constant_force_magnitude = 1000;

    expect_true(!wheelio_bridge::wheel_state_has_effects(empty), "empty payload has no force effects");
    expect_true(!wheelio_bridge::wheel_state_has_effects(with_led), "LED-only payload has no force effects");
    expect_true(wheelio_bridge::wheel_state_has_effects(with_force), "constant force payload has force effects");

    expect_true(wheelio_bridge::wheel_state_equal(empty, empty), "empty payloads compare equal");
    expect_true(!wheelio_bridge::wheel_state_equal(empty, with_led), "LED payload differs from empty payload");
    expect_true(!wheelio_bridge::wheel_state_equal(with_led, with_force), "LED and force payloads differ");
}

void test_directinput_constant_force() {
    wheelio_bridge::DirectInputEffectState state{};
    state.started = true;
    state.start_time_us = 1000;
    state.constant_magnitude = 5000;

    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 1500),
        5000,
        "constant force maps through unchanged at nominal gain");

    state.effect_gain = 5000;
    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 1500),
        2500,
        "effect gain scales constant force");
}

void test_directinput_direction_projection() {
    wheelio_bridge::DirectInputEffectState state{};
    state.started = true;
    state.constant_magnitude = wheelio_bridge::kDirectInputNominalMax;

    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 0),
        10000,
        "single-axis polar direction does not cancel force");

    state.axis_count = 2;
    state.direction_mode = wheelio_bridge::DirectInputDirectionMode::polar;
    state.direction[0] = 0;
    expect_near(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 0),
        0,
        1,
        "polar north has no steering-axis projection");

    state.direction[0] = 9000;
    expect_near(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 0),
        10000,
        1,
        "polar east maps to positive steering force");

    state.direction[0] = 27000;
    expect_near(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 0),
        -10000,
        1,
        "polar west maps to negative steering force");
}

void test_directinput_envelope_and_duration() {
    wheelio_bridge::DirectInputEffectState state{};
    state.started = true;
    state.start_time_us = 1000;
    state.duration = 1000;
    state.iterations = 1;
    state.constant_magnitude = wheelio_bridge::kDirectInputNominalMax;
    state.envelope.enabled = true;
    state.envelope.attack_level = 0;
    state.envelope.attack_time = 1000;

    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 1500),
        5000,
        "attack envelope ramps force");

    expect_true(
        !wheelio_bridge::directinput_effect_is_temporally_active(state, 2500),
        "finite duration effect expires");

    state.iterations = 2;
    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::constant, state, wheelio_bridge::kDirectInputNominalMax, 2500),
        5000,
        "attack envelope is re-articulated on each iteration");
}

void test_directinput_runtime_tick_detection() {
    wheelio_bridge::DirectInputEffectState state{};
    state.started = true;
    state.constant_magnitude = 5000;
    expect_true(
        !wheelio_bridge::directinput_effect_needs_runtime_tick(
            wheelio_bridge::DirectInputEffectKind::constant, state, 0),
        "steady infinite constant force does not need ticking");

    state.duration = 1000;
    state.iterations = 1;
    expect_true(
        wheelio_bridge::directinput_effect_needs_runtime_tick(
            wheelio_bridge::DirectInputEffectKind::constant, state, 0),
        "finite constant force needs ticking so expiration is sent");

    state.duration = wheelio_bridge::kDirectInputInfiniteDuration;
    state.envelope.enabled = true;
    expect_true(
        wheelio_bridge::directinput_effect_needs_runtime_tick(
            wheelio_bridge::DirectInputEffectKind::constant, state, 0),
        "enveloped constant force needs ticking");
}

void test_directinput_periodic_force() {
    wheelio_bridge::DirectInputEffectState state{};
    state.started = true;
    state.periodic_magnitude = wheelio_bridge::kDirectInputNominalMax;
    state.periodic_period = 1000;

    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::square, state, wheelio_bridge::kDirectInputNominalMax, 100),
        10000,
        "square wave first half is positive");
    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::square, state, wheelio_bridge::kDirectInputNominalMax, 600),
        -10000,
        "square wave second half is negative");

    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::triangle, state, wheelio_bridge::kDirectInputNominalMax, 0),
        10000,
        "triangle wave starts at positive peak");
    expect_near(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::triangle, state, wheelio_bridge::kDirectInputNominalMax, 250),
        0,
        1,
        "triangle wave crosses zero at quarter period");
    expect_eq(
        wheelio_bridge::directinput_compute_force(
            wheelio_bridge::DirectInputEffectKind::triangle, state, wheelio_bridge::kDirectInputNominalMax, 500),
        -10000,
        "triangle wave reaches negative peak at half period");
}

void test_directinput_payload_builder() {
    wheelio_bridge::DirectInputEffectState spring{};
    spring.started = true;
    spring.conditions[0].positive_coefficient = 10000;
    spring.conditions[0].positive_saturation = 10000;
    spring.conditions[0].deadband = 10000;
    spring.conditions[1].negative_coefficient = -10000;
    spring.conditions[1].negative_saturation = 10000;
    spring.conditions[1].deadband = 10000;

    wheelio_bridge::DirectInputEffectView effects[] = {
        {wheelio_bridge::DirectInputEffectKind::spring, &spring},
    };
    auto payload = wheelio_bridge::build_directinput_payload(
        effects, 1, wheelio_bridge::kDirectInputNominalMax, false, false, 0);

    expect_true(payload.custom_spring_enabled != 0, "spring effect enables spring payload");
    expect_eq(payload.spring_k1, 15, "spring positive coefficient scales to nibble");
    expect_eq(payload.spring_k2, 15, "spring negative coefficient scales to nibble");
    expect_eq(payload.spring_clip, 255, "spring saturation scales to clip byte");

    spring.conditions[0].deadband = 1000;
    spring.conditions[0].offset = 1000;
    spring.conditions[1].deadband = 1000;
    spring.conditions[1].offset = 1000;
    payload = wheelio_bridge::build_directinput_payload(
        effects, 1, wheelio_bridge::kDirectInputNominalMax, false, false, 0);
    expect_eq(payload.spring_deadband_left, 0, "spring left deadband accounts for positive offset");
    expect_eq(payload.spring_deadband_right, 3, "spring right deadband accounts for positive offset");

    payload = wheelio_bridge::build_directinput_payload(
        nullptr, 0, wheelio_bridge::kDirectInputNominalMax, true, false, 0);
    expect_true(payload.autocenter_enabled != 0, "autocenter fallback is emitted while idle");
    expect_true(payload.autocenter_force > 0, "autocenter fallback carries force");

    // Autocenter must not stack under the game's own forces.
    wheelio_bridge::DirectInputEffectState road{};
    road.started = true;
    road.constant_magnitude = 3000;
    wheelio_bridge::DirectInputEffectView road_effects[] = {
        {wheelio_bridge::DirectInputEffectKind::constant, &road},
    };
    payload = wheelio_bridge::build_directinput_payload(
        road_effects, 1, wheelio_bridge::kDirectInputNominalMax, true, false, 0);
    expect_true(payload.autocenter_enabled == 0, "no autocenter fallback while a force is playing");

    payload = wheelio_bridge::build_directinput_payload(
        effects, 1, wheelio_bridge::kDirectInputNominalMax, false, true, 0);
    expect_true(!wheelio_bridge::wheel_state_has_effects(payload), "disabled output clears payload");
}

void test_directinput_constant_force_summing() {
    // Two simultaneous constant forces (e.g. the road force plus a lane-assist
    // nudge) must add together, not overwrite each other.
    wheelio_bridge::DirectInputEffectState road{};
    road.started = true;
    road.constant_magnitude = 3000;

    wheelio_bridge::DirectInputEffectState lane_assist{};
    lane_assist.started = true;
    lane_assist.constant_magnitude = 2000;

    wheelio_bridge::DirectInputEffectView effects[] = {
        {wheelio_bridge::DirectInputEffectKind::constant, &road},
        {wheelio_bridge::DirectInputEffectKind::constant, &lane_assist},
    };
    auto payload = wheelio_bridge::build_directinput_payload(
        effects, 2, wheelio_bridge::kDirectInputNominalMax, false, false, 0);

    expect_true(payload.constant_force_enabled != 0, "summed constant force is enabled");
    expect_eq(payload.constant_force_magnitude, 5000, "two constant forces sum (road + lane assist)");

    // Opposing forces partially cancel rather than one being dropped.
    lane_assist.constant_magnitude = -1000;
    payload = wheelio_bridge::build_directinput_payload(
        effects, 2, wheelio_bridge::kDirectInputNominalMax, false, false, 0);
    expect_eq(payload.constant_force_magnitude, 2000, "opposing constant forces net out");
}

}  // namespace

int main() {
    test_constant_mapping();
    test_slew_limiter();
    test_payload_helpers();
    test_directinput_constant_force();
    test_directinput_direction_projection();
    test_directinput_envelope_and_duration();
    test_directinput_runtime_tick_detection();
    test_directinput_periodic_force();
    test_directinput_payload_builder();
    test_directinput_constant_force_summing();
    std::cout << "wheelio pure mapping tests passed\n";
    return 0;
}
