#pragma once

#include <SDL3/SDL_scancode.h>

#include <array>
#include <cstddef>

namespace frontend {

// Host keys acting as a digital joystick (for Amiga port 2):
//   arrows or WASD      directions
//   Left Ctrl, Space,   fire
//   Left Alt
// Each key is tracked separately, so releasing one of two keys held for the
// same direction keeps it held. Opposite directions cancel out, as a real
// joystick cannot report both.
class KeyboardJoystick {
public:
    struct State {
        bool up = false;
        bool down = false;
        bool left = false;
        bool right = false;
        bool fire = false;
    };

    // Returns true if the key is one of the joystick keys (then consumed).
    bool key(SDL_Scancode scancode, bool down) noexcept {
        for (size_t i = 0; i < kKeys.size(); ++i) {
            if (kKeys[i].scancode != scancode) continue;
            held_[i] = down;
            return true;
        }
        return false;
    }

    [[nodiscard]] State state() const noexcept {
        State s;
        for (size_t i = 0; i < kKeys.size(); ++i) {
            if (!held_[i]) continue;
            switch (kKeys[i].control) {
                case Control::Up: s.up = true; break;
                case Control::Down: s.down = true; break;
                case Control::Left: s.left = true; break;
                case Control::Right: s.right = true; break;
                case Control::Fire: s.fire = true; break;
            }
        }
        if (s.up && s.down) s.up = s.down = false;
        if (s.left && s.right) s.left = s.right = false;
        return s;
    }

    void release_all() noexcept { held_.fill(false); }

private:
    enum class Control { Up, Down, Left, Right, Fire };
    struct Binding {
        SDL_Scancode scancode;
        Control control;
    };
    static constexpr std::array<Binding, 11> kKeys = {{
        {SDL_SCANCODE_UP, Control::Up},       {SDL_SCANCODE_W, Control::Up},
        {SDL_SCANCODE_DOWN, Control::Down},   {SDL_SCANCODE_S, Control::Down},
        {SDL_SCANCODE_LEFT, Control::Left},   {SDL_SCANCODE_A, Control::Left},
        {SDL_SCANCODE_RIGHT, Control::Right}, {SDL_SCANCODE_D, Control::Right},
        {SDL_SCANCODE_LCTRL, Control::Fire},  {SDL_SCANCODE_SPACE, Control::Fire},
        {SDL_SCANCODE_LALT, Control::Fire},
    }};

    std::array<bool, kKeys.size()> held_{};
};

}  // namespace frontend
