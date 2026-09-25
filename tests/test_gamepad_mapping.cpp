#include <cstdint>
#include <memory>

#include "core/cia.hpp"
#include "core/custom_registers.hpp"
#include "core/machine.hpp"
#include "core/paula.hpp"
#include "frontend/gamepad_mapping.hpp"
#include "test_framework.hpp"

namespace {

using frontend::KeyHolds;
using frontend::pad_action;
using Kind = frontend::PadAction::Kind;
namespace key = frontend::amiga_key;

void test_button_layout() {
    const auto start = pad_action(SDL_GAMEPAD_BUTTON_START);
    EXPECT(start.kind == Kind::Keys && start.key_count == 2);
    EXPECT(start.keys[0] == key::kReturn && start.keys[1] == key::kSpace);

    const auto back = pad_action(SDL_GAMEPAD_BUTTON_BACK);
    EXPECT(back.kind == Kind::Keys && back.key_count == 1 && back.keys[0] == key::kEscape);
    EXPECT(pad_action(SDL_GAMEPAD_BUTTON_EAST).keys[0] == key::kSpace);
    EXPECT(pad_action(SDL_GAMEPAD_BUTTON_WEST).keys[0] == key::kY);
    EXPECT(pad_action(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER).kind == Kind::LeftMouse);
    EXPECT(pad_action(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER).kind == Kind::RightMouse);

    // The joystick buttons stay the joystick's.
    EXPECT(pad_action(SDL_GAMEPAD_BUTTON_SOUTH).kind == Kind::None);
    EXPECT(pad_action(SDL_GAMEPAD_BUTTON_DPAD_UP).kind == Kind::None);
}

void test_shared_keys() {
    KeyHolds holds;
    EXPECT(holds.press(key::kSpace));    // keyboard Space: key goes down
    EXPECT(!holds.press(key::kSpace));   // pad East too: already down, nothing sent
    EXPECT(!holds.release(key::kSpace)); // keyboard lets go: still held by the pad
    EXPECT(holds.held(key::kSpace));
    EXPECT(holds.release(key::kSpace));  // pad lets go: key goes up
    EXPECT(!holds.release(key::kSpace)); // no underflow
}

// START: Return and Space reach the Amiga keyboard; shoulders: the mouse buttons.
void test_pad_actions_reach_the_hardware() {
    auto m = std::make_unique<amiga::Machine>();
    const auto start = pad_action(SDL_GAMEPAD_BUTTON_START);
    for (uint8_t i = 0; i < start.key_count; ++i) m->key_event(start.keys[i], true);
    EXPECT(m->keyboard().pending() == 2);

    m->mouse_buttons(pad_action(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER).kind == Kind::LeftMouse, false, false);
    EXPECT((m->bus().read8(0xBFE001) & amiga::Cias::kPraFir0) == 0);  // CIA-A PA6 low
    m->mouse_buttons(false, pad_action(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER).kind == Kind::RightMouse, false);
    EXPECT((m->bus().read8(0xBFE001) & amiga::Cias::kPraFir0) != 0);
    EXPECT((m->bus().read16(amiga::MemoryBus::kCustomBase + amiga::reg::kPotGoR) & amiga::Paula::kDatLy) == 0);
}

}  // namespace

void run_gamepad_mapping_tests() {
    test_button_layout();
    test_shared_keys();
    test_pad_actions_reach_the_hardware();
}
