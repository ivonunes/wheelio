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
    expect_eq(sizeof(WheelStatePayload), std::size_t{19}, "WheelStatePayload size");
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
    // payload_size (u32 LE) at offset 8 == 19.
    expect_eq(bytes[8], std::uint8_t{19}, "payload_size byte 0");
    expect_eq(bytes[9], std::uint8_t{0}, "payload_size byte 1");
}

void expect_le16(const std::uint8_t* bytes, std::size_t offset, std::uint16_t value, const std::string& name) {
    expect_eq(bytes[offset], static_cast<std::uint8_t>(value & 0xFF), name + " low byte at offset " + std::to_string(offset));
    expect_eq(bytes[offset + 1], static_cast<std::uint8_t>(value >> 8), name + " high byte");
}

void test_wheel_state_offsets_and_roundtrip() {
    using namespace wheelio_bridge;
    WheelStatePayload payload{};
    payload.constant_force = -1234;
    payload.spring_enabled = 1;
    payload.spring_offset = -500;
    payload.spring_deadband = 300;
    payload.spring_positive_coefficient = 7000;
    payload.spring_negative_coefficient = -6000;
    payload.spring_positive_saturation = 10000;
    payload.spring_negative_saturation = 9000;
    payload.damper = 4000;
    payload.friction = 2500;

    std::uint8_t bytes[sizeof(WheelStatePayload)];
    std::memcpy(bytes, &payload, sizeof(bytes));

    // Every field at its fixed offset, little-endian, so reordering or
    // retyping one is caught.
    expect_le16(bytes, 0, static_cast<std::uint16_t>(std::int16_t{-1234}), "constant_force");
    expect_eq(bytes[2], std::uint8_t{1}, "spring_enabled at offset 2");
    expect_le16(bytes, 3, static_cast<std::uint16_t>(std::int16_t{-500}), "spring_offset");
    expect_le16(bytes, 5, 300, "spring_deadband");
    expect_le16(bytes, 7, 7000, "spring_positive_coefficient");
    expect_le16(bytes, 9, static_cast<std::uint16_t>(std::int16_t{-6000}), "spring_negative_coefficient");
    expect_le16(bytes, 11, 10000, "spring_positive_saturation");
    expect_le16(bytes, 13, 9000, "spring_negative_saturation");
    expect_le16(bytes, 15, 4000, "damper");
    expect_le16(bytes, 17, 2500, "friction");

    // Round-trip the raw bytes back into a struct and compare every field.
    WheelStatePayload decoded{};
    std::memcpy(&decoded, bytes, sizeof(decoded));
    expect(std::memcmp(&payload, &decoded, sizeof(payload)) == 0, "wheel state round-trips byte-for-byte");
}

void test_has_effects() {
    using namespace wheelio_bridge;
    WheelStatePayload state{};
    expect(!has_effects(state), "an empty state asks nothing of the wheel");
    state.friction = 1;
    expect(has_effects(state), "friction alone is an effect");
    state = {};
    state.spring_enabled = 1;
    expect(has_effects(state), "a spring is an effect");
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
    spring.spring_enabled = 1;
    spring.spring_positive_coefficient = 5;
    desired.set(spring);
    expect(desired.needs_delivery(), "a new state needs delivering");
    // The delivery failed: it stays pending.
    expect(desired.needs_delivery(), "a failed delivery stays pending");
    desired.delivered();
    expect(!desired.needs_delivery(), "a delivered state is done");

    // The receiver lost it (reconnect, wheel reopened): the same state again.
    desired.redeliver();
    expect(desired.needs_delivery(), "a lost state is delivered again");
    expect_eq(desired.state().spring_positive_coefficient, std::int16_t{5}, "the redelivered state is the last one set");

    desired.clear();
    expect(!desired.needs_delivery() && !desired.has_state(), "a stop leaves nothing to deliver");
    desired.redeliver();
    expect(!desired.needs_delivery(), "nothing comes back after a stop");
}

int main() {
    test_sizes();
    test_header_byte_layout();
    test_wheel_state_offsets_and_roundtrip();
    test_has_effects();
    test_desired_state_is_redelivered_until_it_lands();

    if (g_failures != 0) {
        std::cerr << g_failures << " protocol test(s) failed\n";
        return 1;
    }
    std::cout << "protocol tests passed\n";
    return 0;
}
