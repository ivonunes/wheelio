#pragma once

#include <cstddef>
#include <cstdint>

namespace wheelio_bridge {

constexpr std::uint32_t kProtocolMagic = 0x4646424D;  // "FFBM"
constexpr std::uint16_t kProtocolVersion = 2;
constexpr std::uint16_t kDefaultPort = 18423;
constexpr std::uint32_t kMaxPayloadSize = 4096;

enum class MessageType : std::uint16_t {
    hello = 1,
    hello_ack = 2,
    apply_wheel_state = 10,
    stop_all = 11,
    ping = 12,
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

// The game's force feedback, summed over its playing effects, in DirectInput's
// own units: forces, coefficients and saturations out of 10000, positions on
// the steering axis from -10000 to 10000. The Mac side turns it into the
// wheel's forces (force_mapping.hpp).
struct WheelStatePayload {
    // A constant push, as the game's constant, ramp and periodic effects add
    // up; positive turns the wheel left.
    std::int16_t constant_force = 0;

    // A spring condition (the game's, or DirectInput's autocentre): pushes
    // towards `spring_offset` once the wheel is more than `spring_deadband`
    // from it, by the coefficient on that side, up to that side's saturation.
    std::uint8_t spring_enabled = 0;
    std::int16_t spring_offset = 0;
    std::uint16_t spring_deadband = 0;
    std::int16_t spring_positive_coefficient = 0;
    std::int16_t spring_negative_coefficient = 0;
    std::uint16_t spring_positive_saturation = 0;
    std::uint16_t spring_negative_saturation = 0;

    // Damper (and inertia) and friction conditions, by their stronger side.
    std::uint16_t damper = 0;
    std::uint16_t friction = 0;
};

#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 12, "Unexpected MessageHeader size");
static_assert(sizeof(HelloPayload) == 68, "Unexpected HelloPayload size");
static_assert(sizeof(HelloAckPayload) == 68, "Unexpected HelloAckPayload size");
static_assert(sizeof(WheelStatePayload) == 19, "Unexpected WheelStatePayload size");

// Whether a state asks anything of the wheel.
inline bool has_effects(const WheelStatePayload& state) {
    return state.constant_force != 0 || state.spring_enabled != 0 || state.damper != 0 || state.friction != 0;
}

template <typename T>
constexpr std::size_t payload_size() {
    return sizeof(T);
}

}  // namespace wheelio_bridge
