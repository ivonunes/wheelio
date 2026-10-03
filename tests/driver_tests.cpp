#include "command.hpp"
#include "constants.hpp"
#include "ffb_command_plan.hpp"
#include "wheelio_bridge_protocol.hpp"
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

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

void expect_bytes(const Command& command, std::initializer_list<int> expected, const std::string& message) {
    bool ok = expected.size() == command.size();
    std::size_t index = 0;
    for (int byte : expected) {
        if (index >= command.size() || command.raw()[index] != static_cast<std::uint8_t>(byte)) {
            ok = false;
        }
        ++index;
    }
    if (!ok) {
        std::cerr << "FAIL: " << message << " got bytes:";
        for (std::size_t i = 0; i < command.size(); ++i) {
            std::cerr << ' ' << static_cast<int>(command.raw()[i]);
        }
        std::cerr << '\n';
        std::exit(1);
    }
}

// The force-level byte of the constant-force command in `commands`, or -1.
int constant_force_level(const std::vector<Command>& commands) {
    for (const auto& command : commands) {
        if (command.raw()[0] == (g923_commands::SLOT_CONSTANT | g923_commands::OP_DOWNLOAD_AND_PLAY) &&
            command.raw()[1] == g923_commands::EFFECT_CONSTANT) {
            return command.raw()[2];
        }
    }
    return -1;
}

bool contains_led_command(const std::vector<Command>& commands) {
    for (const auto& command : commands) {
        if (command.raw()[0] == g923_commands::SET_LED_PATTERN) {
            return true;
        }
    }
    return false;
}

wheelio_bridge::WheelStatePayload constant_payload(std::int16_t magnitude) {
    wheelio_bridge::WheelStatePayload payload{};
    payload.constant_force_enabled = 1;
    payload.constant_force_magnitude = magnitude;
    return payload;
}

// --- CommandBuilder: exact lg4ff byte encoding (the wheel protocol contract) ---

void test_command_builder_golden_bytes() {
    expect_bytes(CommandBuilder::create_disable_autocenter(), {0xF5, 0, 0, 0, 0, 0, 0}, "disable autocenter");
    expect_bytes(CommandBuilder::create_enable_autocenter(), {0xF4, 0, 0, 0, 0, 0, 0}, "enable autocenter");
    expect_bytes(CommandBuilder::create_stop_forces(), {0xF3, 0x00, 0, 0, 0, 0, 0}, "stop forces");
    expect_bytes(CommandBuilder::create_led_pattern(0x07), {0xF8, 0x12, 0x07, 0, 0, 0, 0}, "led pattern");
    expect_bytes(CommandBuilder::create_autocenter_spring(2, 3, 0x30),
                 {0xFE, 0x0D, 0x02, 0x03, 0x30, 0x00, 0x00}, "autocenter spring");
    // Byte 0 = slot mask | operation. One slot per effect type so the wheel
    // sums them instead of one replacing another.
    expect_bytes(CommandBuilder::create_constant_force(168),
                 {0x11, 0x00, 168, 0x00, 0x00, 0x00, 0x00}, "constant force in slot 1 only");
    // k1/k2 and s1/s2 are packed two-per-byte: (k2<<4)|k1, (s2<<4)|s1.
    expect_bytes(CommandBuilder::create_custom_spring(1, 2, 3, 4, 5, 6, 7),
                 {0x21, 0x01, 1, 2, 0x43, 0x65, 7}, "custom spring in slot 2 packs coefficient nibbles");
    expect_bytes(CommandBuilder::create_damper(1, 2, 3, 4), {0x41, 0x02, 1, 3, 2, 4, 0x00}, "damper in slot 3");
    expect_bytes(CommandBuilder::create_stop_constant_force(), {0x13, 0, 0, 0, 0, 0, 0}, "stop constant slot");
    expect_bytes(CommandBuilder::create_stop_spring(), {0x23, 0, 0, 0, 0, 0, 0}, "stop spring slot");
    expect_bytes(CommandBuilder::create_stop_damper(), {0x43, 0, 0, 0, 0, 0, 0}, "stop damper slot");
}

// --- plan_wheel_commands: mapping, tuning, and redundant-write suppression ---

void test_plan_constant_mapping_and_center() {
    FfbPlanState state{};
    auto commands = plan_wheel_commands(constant_payload(10000), FfbTuning{}, true, state);
    expect_eq(constant_force_level(commands), 255, "max force maps to center (128) + 127");
}

void test_plan_force_gain_scales() {
    FfbPlanState state{};
    FfbTuning tuning{};
    tuning.force_gain = 0.5;
    // magnitude 10000 -> level 127; gain 0.5 -> 64 (rounded); raw = 128 + 64 = 192.
    auto commands = plan_wheel_commands(constant_payload(10000), tuning, true, state);
    expect_eq(constant_force_level(commands), 192, "force gain scales the constant level");
}

void test_plan_suppresses_redundant_writes() {
    FfbPlanState state{};
    const auto payload = constant_payload(5000);
    auto first = plan_wheel_commands(payload, FfbTuning{}, true, state);
    expect_true(!first.empty(), "first apply emits commands");
    auto second = plan_wheel_commands(payload, FfbTuning{}, true, state);
    expect_true(second.empty(), "an unchanged payload emits no commands");
}

void test_plan_reapplies_on_tuning_change() {
    FfbPlanState state{};
    const auto payload = constant_payload(10000);  // level 127 (saturated, mapping-independent)
    plan_wheel_commands(payload, FfbTuning{}, true, state);
    FfbTuning changed{};
    changed.force_gain = 0.5;  // 127 * 0.5 = 64 (rounded) -> raw 192
    auto commands = plan_wheel_commands(payload, changed, true, state);
    expect_true(!commands.empty(), "a tuning change re-applies despite an identical payload");
    expect_eq(constant_force_level(commands), 192, "re-applied level reflects the new gain");
}

void test_plan_no_effect_stops() {
    FfbPlanState state{};
    wheelio_bridge::WheelStatePayload idle{};
    auto commands = plan_wheel_commands(idle, FfbTuning{}, true, state);
    expect_eq(static_cast<int>(commands.size()), 2, "a no-effect payload emits the stop sequence");
    expect_bytes(commands[0], {0xF3, 0x00, 0, 0, 0, 0, 0}, "stop: every slot off");
    expect_bytes(commands[1], {0xF5, 0, 0, 0, 0, 0, 0}, "stop: autocenter off");
}

void test_plan_smoothing_limits_step() {
    FfbPlanState state{};
    FfbTuning tuning{};
    tuning.smoothing = 0.8;  // small per-update step
    plan_wheel_commands(constant_payload(500), tuning, true, state);  // small first level
    auto commands = plan_wheel_commands(constant_payload(10000), tuning, true, state);  // jump toward full
    const int level = constant_force_level(commands);
    // With heavy smoothing the output ramps toward full instead of jumping to it,
    // so it lands above center (128) but well short of full (255).
    expect_true(level > 128 && level < 255, "smoothing limits the per-update step");
}

void test_plan_led_gating() {
    wheelio_bridge::WheelStatePayload payload = constant_payload(5000);
    payload.led_pattern_enabled = 1;
    payload.led_pattern = 0x0F;

    FfbPlanState with_leds{};
    expect_true(contains_led_command(plan_wheel_commands(payload, FfbTuning{}, true, with_leds)),
                "LED command emitted when the wheel supports LEDs");

    FfbPlanState no_leds{};
    expect_true(!contains_led_command(plan_wheel_commands(payload, FfbTuning{}, false, no_leds)),
                "no LED command when the wheel lacks LEDs");
}

// --- registry: which devices are recognized as supported wheels ---

void test_wheel_profile_registry() {
    const WheelProfile* g923 = find_wheel_profile(0x046D, 0xC266);
    expect_true(g923 != nullptr, "G923 (PS) is a known wheel");
    expect_true(g923->force_feedback_supported, "G923 supports force feedback");
    expect_true(find_wheel_profile(0x046D, 0x0000) == nullptr, "an unknown Logitech product is not a wheel");
    expect_true(find_wheel_profile(0x1234, 0xC266) == nullptr, "an unknown vendor is not a wheel");
}

}  // namespace

int main() {
    test_command_builder_golden_bytes();
    test_plan_constant_mapping_and_center();
    test_plan_force_gain_scales();
    test_plan_suppresses_redundant_writes();
    test_plan_reapplies_on_tuning_change();
    test_plan_no_effect_stops();
    test_plan_smoothing_limits_step();
    test_plan_led_gating();
    test_wheel_profile_registry();
    std::cout << "wheelio driver/command tests passed\n";
    return 0;
}
