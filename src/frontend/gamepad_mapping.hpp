#pragma once

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdint>

namespace frontend {

// Gamepad buttons beyond the joystick (D-pad/left stick = directions, South
// = fire): shortcuts to the Amiga keys and mouse buttons games commonly need
// to skip intros, confirm prompts or drive cracktro menus with a controller.
//   Start          Return + Space (either skips most intros)
//   Back/Select    Esc
//   East  (B/O)    Space
//   West  (X/[])   Y (confirmation prompts)
//   Left shoulder  left mouse button
//   Right shoulder right mouse button
struct PadAction {
    enum class Kind : uint8_t { None, Keys, LeftMouse, RightMouse };
    Kind kind = Kind::None;
    std::array<uint8_t, 2> keys{};  // Amiga raw keycodes
    uint8_t key_count = 0;
};

namespace amiga_key {
inline constexpr uint8_t kY = 0x15;
inline constexpr uint8_t kSpace = 0x40;
inline constexpr uint8_t kReturn = 0x44;
inline constexpr uint8_t kEscape = 0x45;
}  // namespace amiga_key

[[nodiscard]] constexpr PadAction pad_action(SDL_GamepadButton button) noexcept {
    using Kind = PadAction::Kind;
    switch (button) {
        case SDL_GAMEPAD_BUTTON_START: return {Kind::Keys, {amiga_key::kReturn, amiga_key::kSpace}, 2};
        case SDL_GAMEPAD_BUTTON_BACK: return {Kind::Keys, {amiga_key::kEscape, 0}, 1};
        case SDL_GAMEPAD_BUTTON_EAST: return {Kind::Keys, {amiga_key::kSpace, 0}, 1};
        case SDL_GAMEPAD_BUTTON_WEST: return {Kind::Keys, {amiga_key::kY, 0}, 1};
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return {Kind::LeftMouse, {}, 0};
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return {Kind::RightMouse, {}, 0};
        default: return {};
    }
}

// Several host inputs can hold the same Amiga key (e.g. Space on the keyboard
// and East on the pad): the key goes down with the first and up with the last.
class KeyHolds {
public:
    // True if the key has just gone down (send a key press).
    bool press(uint8_t code) noexcept { return code < holds_.size() && holds_[code]++ == 0; }
    // True if the key has just gone up (send a key release).
    bool release(uint8_t code) noexcept {
        if (code >= holds_.size() || holds_[code] == 0) return false;
        return --holds_[code] == 0;
    }
    [[nodiscard]] bool held(uint8_t code) const noexcept { return code < holds_.size() && holds_[code] != 0; }

private:
    std::array<uint8_t, 128> holds_{};
};

}  // namespace frontend
