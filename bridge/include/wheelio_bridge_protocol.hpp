#pragma once

#include <cstddef>
#include <cstdint>

namespace wheelio_bridge {

constexpr std::uint32_t kProtocolMagic = 0x4646424D;  // "FFBM"
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kDefaultPort = 18423;
constexpr std::uint32_t kMaxPayloadSize = 4096;

enum class MessageType : std::uint16_t {
    hello = 1,
    hello_ack = 2,
    apply_wheel_state = 10,
    stop_all = 11,
    ping = 12,
    set_led_pattern = 13,
};

// Wire format is little-endian. Both ends are little-endian today (Apple
// Silicon / Intel macOS, and the mingw x86_64 Windows DLL), so the packed
// structs below are exchanged byte-for-byte without conversion. If a
// big-endian peer is ever introduced, add explicit byte-swapping at the
// socket boundary.
#pragma pack(push, 1)

struct MessageHeader {
    std::uint32_t magic = kProtocolMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t type = 0;
    std::uint32_t payload_size = 0;
};

struct HelloPayload {
    char client_name[64] = {0};
    std::uint32_t process_id = 0;
};

struct HelloAckPayload {
    std::uint8_t accepted = 0;
    std::uint8_t wheel_connected = 0;
    std::uint16_t server_port = kDefaultPort;
    char wheel_name[64] = {0};
};

struct WheelStatePayload {
    std::uint8_t autocenter_enabled = 0;
    std::uint8_t autocenter_force = 0;
    std::uint8_t autocenter_slope = 0;

    std::uint8_t custom_spring_enabled = 0;
    std::uint8_t spring_deadband_left = 0;
    std::uint8_t spring_deadband_right = 0;
    std::uint8_t spring_k1 = 0;
    std::uint8_t spring_k2 = 0;
    std::uint8_t spring_sat1 = 0;
    std::uint8_t spring_sat2 = 0;
    std::uint8_t spring_clip = 0;

    std::uint8_t damper_enabled = 0;
    std::uint8_t damper_force_positive = 0;
    std::uint8_t damper_force_negative = 0;
    std::uint8_t damper_saturation_positive = 0;
    std::uint8_t damper_saturation_negative = 0;

    std::uint8_t constant_force_enabled = 0;
    std::int16_t constant_force_magnitude = 0;

    std::uint8_t led_pattern_enabled = 0;
    std::uint8_t led_pattern = 0;
};

struct LedPatternPayload {
    std::uint8_t pattern = 0;
};

#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 12, "Unexpected MessageHeader size");
static_assert(sizeof(HelloPayload) == 68, "Unexpected HelloPayload size");
static_assert(sizeof(HelloAckPayload) == 68, "Unexpected HelloAckPayload size");
static_assert(sizeof(WheelStatePayload) == 21, "Unexpected WheelStatePayload size");
static_assert(sizeof(LedPatternPayload) == 1, "Unexpected LedPatternPayload size");

template <typename T>
constexpr std::size_t payload_size() {
    return sizeof(T);
}

}  // namespace wheelio_bridge
