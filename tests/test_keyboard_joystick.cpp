#include <cstdint>
#include <memory>

#include "core/cia.hpp"
#include "core/custom_registers.hpp"
#include "core/machine.hpp"
#include "frontend/keyboard_joystick.hpp"
#include "test_framework.hpp"

namespace {

using frontend::KeyboardJoystick;

// What a game sees on port 2 after the keyboard joystick state is applied.
struct Port2 {
    uint16_t joy1dat;
    bool fire;
};

Port2 apply(amiga::Machine& m, const KeyboardJoystick& k) {
    const KeyboardJoystick::State s = k.state();
    m.joystick(s.up, s.down, s.left, s.right, s.fire);
    return {m.bus().read16(amiga::MemoryBus::kCustomBase + amiga::reg::kJoy1Dat),
            (m.bus().read8(0xBFE001) & amiga::Cias::kPraFir1) == 0};
}

void test_key_mapping() {
    KeyboardJoystick k;
    EXPECT(!k.key(SDL_SCANCODE_E, true));  // not a joystick key: left for the Amiga keyboard
    for (const auto sc : {SDL_SCANCODE_UP, SDL_SCANCODE_W}) {
        EXPECT(k.key(sc, true) && k.state().up);
        k.key(sc, false);
    }
    for (const auto sc : {SDL_SCANCODE_DOWN, SDL_SCANCODE_S}) {
        EXPECT(k.key(sc, true) && k.state().down);
        k.key(sc, false);
    }
    for (const auto sc : {SDL_SCANCODE_LEFT, SDL_SCANCODE_A}) {
        EXPECT(k.key(sc, true) && k.state().left);
        k.key(sc, false);
    }
    for (const auto sc : {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D}) {
        EXPECT(k.key(sc, true) && k.state().right);
        k.key(sc, false);
    }
    for (const auto sc : {SDL_SCANCODE_LCTRL, SDL_SCANCODE_SPACE, SDL_SCANCODE_LALT}) {
        EXPECT(k.key(sc, true) && k.state().fire);
        k.key(sc, false);
    }
    const auto s = k.state();
    EXPECT(!s.up && !s.down && !s.left && !s.right && !s.fire);
}

void test_held_keys_and_opposites() {
    KeyboardJoystick k;
    k.key(SDL_SCANCODE_LEFT, true);
    k.key(SDL_SCANCODE_A, true);
    k.key(SDL_SCANCODE_LEFT, false);
    EXPECT(k.state().left);  // A still held
    k.key(SDL_SCANCODE_D, true);
    EXPECT(!k.state().left && !k.state().right);  // left + right cancel
    k.release_all();
    EXPECT(!k.state().left && !k.state().right);
}

void test_keys_reach_port_2() {
    auto m = std::make_unique<amiga::Machine>();
    KeyboardJoystick k;
    k.key(SDL_SCANCODE_RIGHT, true);
    EXPECT(apply(*m, k).joy1dat == 0x0003);  // right: bit 1, bit 0 = down ^ right
    k.key(SDL_SCANCODE_UP, true);
    EXPECT(apply(*m, k).joy1dat == 0x0103);  // + up: bit 8 = up ^ left
    k.key(SDL_SCANCODE_RIGHT, false);
    k.key(SDL_SCANCODE_UP, false);
    k.key(SDL_SCANCODE_A, true);
    k.key(SDL_SCANCODE_S, true);
    EXPECT(apply(*m, k).joy1dat == 0x0301);  // left + down: bits 9, 8 and 0
    EXPECT(!apply(*m, k).fire);
    k.key(SDL_SCANCODE_SPACE, true);
    EXPECT(apply(*m, k).fire);               // CIA-A PA7 low
    k.release_all();
    const Port2 idle = apply(*m, k);
    EXPECT(idle.joy1dat == 0 && !idle.fire);
}

}  // namespace

void run_keyboard_joystick_tests() {
    test_key_mapping();
    test_held_keys_and_opposites();
    test_keys_reach_port_2();
}
