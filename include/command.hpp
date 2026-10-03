#pragma once

#include "types.hpp"
#include "constants.hpp"
#include <IOKit/IOReturn.h>
#include <vector>
#include <ctime>

// Logitech (lg4ff) command bytes. Byte 0 is a slot mask in the high nibble and
// an operation in the low nibble: 0x11 downloads and plays into slot 1, 0x13
// stops slot 1, 0xF3 stops all four slots. The wheel runs the four slots as
// independent forces and sums them, so each effect type gets its own slot,
// the same way the Linux driver lays them out.
namespace g923_commands {
    static constexpr std::uint8_t DISABLE_AUTOCENTER = 0xF5;
    static constexpr std::uint8_t ENABLE_AUTOCENTER = 0xF4;
    static constexpr std::uint8_t SET_AUTOCENTER_SPRING = 0xFE;
    static constexpr std::uint8_t AUTOCENTER_SPRING_PARAMS = 0x0D;
    static constexpr std::uint8_t STOP_FORCES = 0xF3;  // every slot
    static constexpr std::uint8_t SET_LED_PATTERN = 0xF8;

    static constexpr std::uint8_t OP_DOWNLOAD_AND_PLAY = 0x01;
    static constexpr std::uint8_t OP_STOP = 0x03;
    static constexpr std::uint8_t SLOT_CONSTANT = 0x10;  // also ramp and periodic, summed by the proxy
    static constexpr std::uint8_t SLOT_SPRING = 0x20;
    static constexpr std::uint8_t SLOT_DAMPER = 0x40;

    static constexpr std::uint8_t EFFECT_CONSTANT = 0x00;
    static constexpr std::uint8_t EFFECT_SPRING = 0x01;
    static constexpr std::uint8_t EFFECT_DAMPER = 0x02;
    static constexpr std::uint8_t EFFECT_TRAPEZOID = 0x06;

    static constexpr std::uint8_t LED_COMMAND_TYPE = 0x12;
}

class CommandBuilder {
public:
    static Command create_disable_autocenter();
    static Command create_enable_autocenter();
    static Command create_autocenter_spring(std::uint8_t k1, std::uint8_t k2, std::uint8_t clip);
    static Command create_constant_force(std::uint8_t force_level);
    static Command create_custom_spring(std::uint8_t d1, std::uint8_t d2, std::uint8_t k1, std::uint8_t k2,
                                        std::uint8_t s1, std::uint8_t s2, std::uint8_t clip);
    static Command create_damper(std::uint8_t k1, std::uint8_t k2, std::uint8_t s1, std::uint8_t s2);
    static Command create_trapezoid(std::uint8_t l1, std::uint8_t l2, std::uint8_t t1, std::uint8_t t2,
                                    std::uint8_t t3, std::uint8_t s);
    static Command create_stop_forces();
    static Command create_stop_constant_force();
    static Command create_stop_spring();
    static Command create_stop_damper();
    static Command create_led_pattern(std::uint8_t pattern);

private:
    static Command create_force_effect_command(std::uint8_t slot, std::uint8_t effect_type,
                                                const std::vector<std::uint8_t>& params);
};
