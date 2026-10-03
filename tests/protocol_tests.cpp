// Verifies the on-the-wire layout of the bridge protocol structs. Both the
// macOS server and the Windows dinput8.dll include the same header, so the real
// risk is a field being reordered/retyped (which a size check alone won't catch)
// silently breaking the two halves. These tests pin sizes, exact byte offsets,
// little-endian encoding, and round-trip equality.
#include "desired_wheel_state.hpp"
#include "wheelio_bridge_protocol.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

template <typename T>
void expect_eq(T actual, T expected, const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " expected " << +expected << " got " << +actual << '\n';
        ++g_failures;
    }
}

void test_sizes() {
    using namespace wheelio_bridge;
    expect_eq(sizeof(MessageHeader), std::size_t{12}, "MessageHeader size");
    expect_eq(sizeof(HelloPayload), std::size_t{68}, "HelloPayload size");
    expect_eq(sizeof(HelloAckPayload), std::size_t{68}, "HelloAckPayload size");
    expect_eq(sizeof(WheelStatePayload), std::size_t{21}, "WheelStatePayload size");
    expect_eq(sizeof(LedPatternPayload), std::size_t{1}, "LedPatternPayload size");
}

void test_header_byte_layout() {
    using namespace wheelio_bridge;
    MessageHeader header{};
    header.type = static_cast<std::uint16_t>(MessageType::apply_wheel_state);
    header.payload_size = sizeof(WheelStatePayload);

    std::uint8_t bytes[sizeof(MessageHeader)];
    std::memcpy(bytes, &header, sizeof(bytes));

    // magic = 0x4646424D, little-endian on the wire -> bytes 4D 42 46 46.
    expect_eq(bytes[0], std::uint8_t{0x4D}, "magic byte 0");
    expect_eq(bytes[1], std::uint8_t{0x42}, "magic byte 1");
    expect_eq(bytes[2], std::uint8_t{0x46}, "magic byte 2");
    expect_eq(bytes[3], std::uint8_t{0x46}, "magic byte 3");
    // version (u16 LE) at offset 4.
    expect_eq(bytes[4], std::uint8_t{kProtocolVersion & 0xFF}, "version low byte");
    expect_eq(bytes[5], std::uint8_t{(kProtocolVersion >> 8) & 0xFF}, "version high byte");
    // type (u16 LE) at offset 6 == 10 (apply_wheel_state).
    expect_eq(bytes[6], std::uint8_t{10}, "type low byte");
    expect_eq(bytes[7], std::uint8_t{0}, "type high byte");
    // payload_size (u32 LE) at offset 8 == 21.
    expect_eq(bytes[8], std::uint8_t{21}, "payload_size byte 0");
    expect_eq(bytes[9], std::uint8_t{0}, "payload_size byte 1");
}

void test_wheel_state_offsets_and_roundtrip() {
    using namespace wheelio_bridge;
    WheelStatePayload payload{};
    payload.autocenter_enabled = 1;
    payload.autocenter_force = 2;
    payload.autocenter_slope = 3;
    payload.custom_spring_enabled = 1;
    payload.spring_clip = 99;
    payload.damper_enabled = 1;
    payload.constant_force_enabled = 1;
    payload.constant_force_magnitude = -1234;  // signed 16-bit
    payload.led_pattern_enabled = 1;
    payload.led_pattern = 0x1F;

    std::uint8_t bytes[sizeof(WheelStatePayload)];
    std::memcpy(bytes, &payload, sizeof(bytes));

    // Spot-check a few fixed offsets so reordering fields is caught.
    expect_eq(bytes[0], std::uint8_t{1}, "autocenter_enabled at offset 0");
    expect_eq(bytes[1], std::uint8_t{2}, "autocenter_force at offset 1");
    // constant_force_magnitude is a little-endian int16 at offset 17.
    const std::uint16_t raw = static_cast<std::uint16_t>(static_cast<std::int16_t>(-1234));
    expect_eq(bytes[17], static_cast<std::uint8_t>(raw & 0xFF), "constant_force_magnitude low byte at offset 17");
    expect_eq(bytes[18], static_cast<std::uint8_t>((raw >> 8) & 0xFF), "constant_force_magnitude high byte at offset 18");
    expect_eq(bytes[19], std::uint8_t{1}, "led_pattern_enabled at offset 19");
    expect_eq(bytes[20], std::uint8_t{0x1F}, "led_pattern at offset 20");

    // Round-trip the raw bytes back into a struct and compare every field.
    WheelStatePayload decoded{};
    std::memcpy(&decoded, bytes, sizeof(decoded));
    expect(std::memcmp(&payload, &decoded, sizeof(payload)) == 0, "wheel state round-trips byte-for-byte");
}

}  // namespace

// A state that failed to arrive, or that the receiver lost, is delivered
// again until it lands; a stop leaves nothing to redeliver.
void test_desired_state_is_redelivered_until_it_lands() {
    using namespace wheelio_bridge;
    DesiredWheelState desired;
    expect(!desired.needs_delivery(), "nothing to deliver before the game sets a state");
    desired.redeliver();
    expect(!desired.needs_delivery(), "redelivering with no state does nothing");

    WheelStatePayload spring{};
    spring.custom_spring_enabled = 1;
    spring.spring_k1 = 5;
    desired.set(spring);
    expect(desired.needs_delivery(), "a new state needs delivering");
    // The delivery failed: it stays pending.
    expect(desired.needs_delivery(), "a failed delivery stays pending");
    desired.delivered();
    expect(!desired.needs_delivery(), "a delivered state is done");

    // The receiver lost it (reconnect, wheel reopened): the same state again.
    desired.redeliver();
    expect(desired.needs_delivery(), "a lost state is delivered again");
    expect_eq(desired.state().spring_k1, std::uint8_t{5}, "the redelivered state is the last one set");

    desired.clear();
    expect(!desired.needs_delivery() && !desired.has_state(), "a stop leaves nothing to deliver");
    desired.redeliver();
    expect(!desired.needs_delivery(), "nothing comes back after a stop");
}

int main() {
    test_sizes();
    test_header_byte_layout();
    test_wheel_state_offsets_and_roundtrip();
    test_desired_state_is_redelivered_until_it_lands();

    if (g_failures != 0) {
        std::cerr << g_failures << " protocol test(s) failed\n";
        return 1;
    }
    std::cout << "protocol tests passed\n";
    return 0;
}
