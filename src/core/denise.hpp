#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/agnus.hpp"
#include "core/custom_registers.hpp"

namespace amiga {

// Denise (OCS 8362) — bitplane display.
//
// Takes the words Agnus fetched for a line and produces ARGB pixels:
//   - 1 to 6 bitplanes, lowres or hires (hires: up to 4 planes on OCS);
//   - 6 planes: Extra Half-Brite (bit 5 halves COLORxx of bits 0-4);
//   - dual playfield: odd planes = playfield 1 (COLOR00-07), even planes =
//     playfield 2 (COLOR08-15), index 0 transparent, BPLCON2 PF2PRI priority;
//   - hold-and-modify (5-6 planes, lowres);
//   - BPLCON1 scroll delays, per playfield;
//   - the display window: outside DIWSTRT/DIWSTOP the border shows COLOR00.
//
// The output covers a fixed viewport, the standard PAL display area: 320
// lowres pixels from H $81 and 256 lines from V $2C. It is 640 pixels wide
// (hires resolution; lowres pixels are doubled). Overscan outside it is
// cropped.
//
// It also holds the port counters: JOY0DAT (mouse, port 0) and JOY1DAT
// (joystick, port 1).
//
// Limitations: sprites and collisions are not emulated, and a whole line is
// rendered at once (register changes in the middle of a line apply to all of it).
class Denise {
public:
    static constexpr unsigned kOutputWidth = 640;  // hires resolution; lowres pixels are doubled
    static constexpr unsigned kDisplayLines = 256;  // PAL, non-interlaced
    static constexpr uint16_t kViewportStartH = 0x81;  // lowres position of column 0
    static constexpr uint16_t kViewportStartV = 0x2C;  // line shown at row 0

    using Row = std::span<uint32_t, kOutputWidth>;

    Denise() noexcept { update_palette(); }

    // OCS colour registers are 12-bit $0RGB; expand each 4-bit channel to 8 bits.
    [[nodiscard]] static constexpr uint32_t rgb12_to_argb(uint16_t rgb) noexcept {
        const uint32_t r = (rgb >> 8) & 0xFu;
        const uint32_t g = (rgb >> 4) & 0xFu;
        const uint32_t b = rgb & 0xFu;
        return 0xFF00'0000u | (r * 0x11u) << 16 | (g * 0x11u) << 8 | (b * 0x11u);
    }

    // Extra Half-Brite: each 4-bit channel shifted right by one.
    [[nodiscard]] static constexpr uint16_t half_brite(uint16_t rgb) noexcept {
        return static_cast<uint16_t>((rgb >> 1) & 0x0777u);
    }

    // Returns false if the register doesn't belong to Denise (or isn't emulated).
    bool write_register(uint16_t offset, uint16_t value) noexcept {
        if (offset >= reg::kColor00 && offset <= reg::kColor31 && (offset & 1u) == 0) {
            const unsigned index = (offset - reg::kColor00) / 2u;
            color_[index] = value & 0x0FFFu;
            palette_[index] = rgb12_to_argb(color_[index]);
            half_brite_palette_[index] = rgb12_to_argb(half_brite(color_[index]));
            return true;
        }
        switch (offset) {
            case reg::kJoyTest:  // sets bits 7-2 of all counters
                for (unsigned port = 0; port < 2; ++port) {
                    mouse_x_[port] = static_cast<uint8_t>((mouse_x_[port] & 3u) | (value & 0xFCu));
                    mouse_y_[port] = static_cast<uint8_t>((mouse_y_[port] & 3u) | ((value >> 8) & 0xFCu));
                }
                return true;
            case reg::kBplCon0: bplcon0_ = value; return true;
            case reg::kBplCon1: bplcon1_ = value & 0x00FFu; return true;
            case reg::kBplCon2: bplcon2_ = value & 0x007Fu; return true;
            default: return false;
        }
    }

    // Returns false if the register isn't a readable Denise register.
    bool read_register(uint16_t offset, uint16_t& value) const noexcept {
        switch (offset) {
            case reg::kJoy0Dat: value = static_cast<uint16_t>(mouse_y_[0] << 8 | mouse_x_[0]); return true;
            case reg::kJoy1Dat: value = static_cast<uint16_t>(mouse_y_[1] << 8 | mouse_x_[1]); return true;
            default: return false;
        }
    }

    // Mouse on port 0: its quadrature counters follow the motion, wrapping at
    // 8 bits. Software reads JOY0DAT and uses the difference between reads.
    void mouse_move(int dx, int dy) noexcept {
        mouse_x_[0] = static_cast<uint8_t>(mouse_x_[0] + dx);
        mouse_y_[0] = static_cast<uint8_t>(mouse_y_[0] + dy);
    }

    // Digital joystick on port 1 (the second port): the switches drive the
    // counter bits directly. HRM encoding: right = X1 (bit 1), left = Y1
    // (bit 9), down = X1 ^ X0 (bit 0), up = Y1 ^ Y0 (bit 8).
    void set_joystick(bool up, bool down, bool left, bool right) noexcept {
        mouse_x_[1] = static_cast<uint8_t>((right ? 2u : 0u) | ((down != right) ? 1u : 0u));
        mouse_y_[1] = static_cast<uint8_t>((left ? 2u : 0u) | ((up != left) ? 1u : 0u));
    }

    [[nodiscard]] uint16_t color(unsigned index) const noexcept { return color_[index & 31u]; }
    [[nodiscard]] uint32_t palette(unsigned index) const noexcept { return palette_[index & 31u]; }
    [[nodiscard]] uint16_t bplcon0() const noexcept { return bplcon0_; }

    // Produces one output line from the words Agnus fetched for it.
    void render_line(const LineFetch& fetch, const DisplayWindow& window, Row out) const noexcept;

private:
    void update_palette() noexcept {
        for (unsigned i = 0; i < color_.size(); ++i) {
            palette_[i] = rgb12_to_argb(color_[i]);
            half_brite_palette_[i] = rgb12_to_argb(half_brite(color_[i]));
        }
    }

    std::array<uint16_t, 32> color_{};
    std::array<uint32_t, 32> palette_{};             // ARGB of COLOR00-31
    std::array<uint32_t, 32> half_brite_palette_{};  // ARGB of COLOR00-31 at half brightness
    uint16_t bplcon0_ = 0;
    uint16_t bplcon1_ = 0;
    uint16_t bplcon2_ = 0;
    std::array<uint8_t, 2> mouse_x_{};  // per port
    std::array<uint8_t, 2> mouse_y_{};
};

}  // namespace amiga
