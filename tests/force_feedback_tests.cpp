#include "dinput_effects.hpp"
#include "force_mapping.hpp"
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
    spring.conditions[0].offset = 1000;
    spring.conditions[0].deadband = 500;
    spring.conditions[0].positive_coefficient = 8000;
    spring.conditions[0].negative_coefficient = 6000;
    spring.conditions[0].positive_saturation = 10000;
    spring.conditions[0].negative_saturation = 9000;
    // A second axis's condition must not leak into the steering's.
    spring.conditions[1].negative_coefficient = 100;
    spring.conditions[1].negative_saturation = 100;

    wheelio_bridge::DirectInputEffectView effects[] = {
        {wheelio_bridge::DirectInputEffectKind::spring, &spring},
    };
    auto payload = wheelio_bridge::build_directinput_payload(
        effects, 1, wheelio_bridge::kDirectInputNominalMax, false, false, 0);

    expect_true(payload.spring_enabled != 0, "spring effect enables the spring");
    expect_eq(payload.spring_offset, 1000, "spring keeps its centre");
    expect_eq(payload.spring_deadband, 500, "spring keeps its deadband");
    expect_eq(payload.spring_positive_coefficient, 8000, "spring positive coefficient passes through");
    expect_eq(payload.spring_negative_coefficient, 6000, "spring negative coefficient is the steering axis's");
    expect_eq(payload.spring_positive_saturation, 10000, "spring positive saturation passes through");
    expect_eq(payload.spring_negative_saturation, 9000, "spring negative saturation is the steering axis's");

    payload = wheelio_bridge::build_directinput_payload(
        effects, 1, wheelio_bridge::kDirectInputNominalMax / 2, false, false, 0);
    expect_eq(payload.spring_positive_coefficient, 4000, "device gain scales the spring");
    expect_eq(payload.spring_positive_saturation, 5000, "device gain scales the spring's saturation");

    wheelio_bridge::DirectInputEffectState damper{};
    damper.started = true;
    damper.conditions[0].positive_coefficient = 3000;
    damper.conditions[0].negative_coefficient = -7000;
    wheelio_bridge::DirectInputEffectState friction = damper;
    friction.conditions[0].negative_coefficient = 2000;
    wheelio_bridge::DirectInputEffectView conditions[] = {
        {wheelio_bridge::DirectInputEffectKind::damper, &damper},
        {wheelio_bridge::DirectInputEffectKind::friction, &friction},
    };
    payload = wheelio_bridge::build_directinput_payload(
        conditions, 2, wheelio_bridge::kDirectInputNominalMax, false, false, 0);
    expect_eq(payload.damper, 7000, "damper takes its stronger side");
    expect_eq(payload.friction, 3000, "friction is its own");

    payload = wheelio_bridge::build_directinput_payload(
        nullptr, 0, wheelio_bridge::kDirectInputNominalMax, true, false, 0);
    expect_true(payload.spring_enabled != 0, "autocentre plays as a spring while idle");
    expect_eq(payload.spring_offset, 0, "autocentre centres the wheel");
    expect_true(payload.spring_positive_coefficient > 0 && payload.spring_negative_coefficient > 0,
                "autocentre pushes back from both sides");
    expect_true(payload.spring_positive_saturation > 0 && payload.spring_positive_saturation < 10000,
                "autocentre is held short of full strength");

    // Autocentre must not stack under the game's own forces.
    wheelio_bridge::DirectInputEffectState road{};
    road.started = true;
    road.constant_magnitude = 3000;
    wheelio_bridge::DirectInputEffectView road_effects[] = {
        {wheelio_bridge::DirectInputEffectKind::constant, &road},
    };
    payload = wheelio_bridge::build_directinput_payload(
        road_effects, 1, wheelio_bridge::kDirectInputNominalMax, true, false, 0);
    expect_true(payload.spring_enabled == 0, "no autocentre while a force is playing");

    payload = wheelio_bridge::build_directinput_payload(
        effects, 1, wheelio_bridge::kDirectInputNominalMax, false, true, 0);
    expect_true(!wheelio_bridge::has_effects(payload), "disabled output clears the state");
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
    expect_eq(payload.constant_force, 5000, "two constant forces sum (road + lane assist)");

    // Opposing forces partially cancel rather than one being dropped.
    lane_assist.constant_magnitude = -1000;
    payload = wheelio_bridge::build_directinput_payload(
        effects, 2, wheelio_bridge::kDirectInputNominalMax, false, false, 0);
    expect_eq(payload.constant_force, 2000, "opposing constant forces net out");
}

bool near(double actual, double expected) {
    return actual > expected - 1e-9 && actual < expected + 1e-9;
}

void test_force_mapping() {
    wheelio_bridge::WheelStatePayload state{};
    FfbTuning tuning;

    expect_true(near(map_wheel_state(state, tuning).force, 0.0), "no state, no force");
    state.constant_force = 5000;
    expect_true(near(map_wheel_state(state, tuning).force, 0.5), "a force maps to its share of full scale");
    state.constant_force = -10000;
    expect_true(near(map_wheel_state(state, tuning).force, -1.0), "full force keeps its sign");

    tuning.force_gain = 1.5;
    expect_true(near(map_wheel_state(state, tuning).force, -1.5), "the force gain scales past full (soft-clipped later)");
    tuning.force_gain = 1.0;

    tuning.min_force = 0.2;
    state.constant_force = 1;
    expect_true(near(map_wheel_state(state, tuning).force, 0.0), "the minimum force leaves the deadzone at zero");
    state.constant_force = -5000;
    expect_true(near(map_wheel_state(state, tuning).force, -0.6), "the minimum force lifts a force above its floor");
    state.constant_force = 10000;
    expect_true(near(map_wheel_state(state, tuning).force, 1.0), "the minimum force keeps full scale");
    tuning.min_force = 0.0;

    state = {};
    state.spring_enabled = 1;
    state.spring_offset = -2000;
    state.spring_deadband = 1000;
    state.spring_positive_coefficient = 5000;
    state.spring_negative_coefficient = 10000;
    state.spring_positive_saturation = 8000;
    state.spring_negative_saturation = 12000;
    tuning.spring_gain = 0.5;
    const WheelForces forces = map_wheel_state(state, tuning);
    expect_true(forces.spring.has_value(), "a spring state plays a spring");
    expect_true(near(forces.spring->centre, -0.2), "the spring's centre is on the steering axis");
    expect_true(near(forces.spring->deadband, 0.1), "the spring's deadband is in the axis's units");
    expect_true(near(forces.spring->positive_stiffness, 0.25), "the spring gain scales the stiffness");
    expect_true(near(forces.spring->negative_stiffness, 0.5), "each side has its own stiffness");
    expect_true(near(forces.spring->positive_saturation, 0.8), "the saturation is a share of full force");
    expect_true(near(forces.spring->negative_saturation, 1.0), "the saturation stops at full force");
    expect_true(!map_wheel_state({}, tuning).spring.has_value(), "no spring state, no spring");

    state = {};
    state.damper = 6000;
    state.friction = 2000;
    tuning.damper_gain = 2.0;
    expect_true(near(map_wheel_state(state, tuning).damper, 1.0), "the damper gain stops at the wheel's strongest");
    expect_true(near(map_wheel_state(state, tuning).friction, 0.4), "the damper gain scales friction too");
}

void test_force_smoothing() {
    ForceSmoothing off;
    expect_true(near(off.step(0.8, 0.01, 0.0), 0.8), "no smoothing passes the force straight through");

    ForceSmoothing heavy;
    const double first = heavy.step(1.0, 0.01, 1.0);
    expect_true(first > 0.0 && first < 0.2, "full smoothing eases a step in");
    double eased = first;
    for (int i = 0; i < 100; ++i) {
        eased = heavy.step(1.0, 0.01, 1.0);
    }
    expect_true(eased > 0.99, "full smoothing reaches the force within a second");
}

}  // namespace

int main() {
    test_directinput_constant_force();
    test_directinput_direction_projection();
    test_directinput_envelope_and_duration();
    test_directinput_runtime_tick_detection();
    test_directinput_periodic_force();
    test_directinput_payload_builder();
    test_directinput_constant_force_summing();
    test_force_mapping();
    test_force_smoothing();
    
    return 0;
}
