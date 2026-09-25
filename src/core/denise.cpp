#include "core/denise.hpp"

#include <algorithm>

namespace amiga {

void Denise::render_line(const LineFetch& fetch, const DisplayWindow& window, Row out) const noexcept {
    if (!fetch.vertical_window) {
        std::ranges::fill(out, palette_[0]);  // vertical border
        return;
    }

    // 1. Serialise each plane onto the line: bit p of pixels[x] is plane p+1.
    //    Column x is at hires position 2 * kViewportStartH + x. The scroll
    //    delay (in lowres pixels) is PF1H for odd planes, PF2H for even ones.
    std::array<uint8_t, kOutputWidth> pixels{};
    const int pixel_width = fetch.hires ? 1 : 2;
    const auto pf1_delay = static_cast<int>(bplcon1_ & 0xFu);
    const auto pf2_delay = static_cast<int>((bplcon1_ >> 4) & 0xFu);

    for (unsigned plane = 0; plane < fetch.planes; ++plane) {
        const int delay = (plane & 1u) == 0 ? pf1_delay : pf2_delay;  // plane 0 = BPL1 (odd)
        int column = 2 * (static_cast<int>(fetch.first_pixel) + delay - kViewportStartH);
        const auto plane_bit = static_cast<uint8_t>(1u << plane);

        for (unsigned word = 0; word < fetch.words; ++word) {
            const uint16_t data = fetch.data[plane][word];
            for (unsigned bit = 0; bit < 16; ++bit, column += pixel_width) {
                if ((data & (0x8000u >> bit)) == 0) continue;
                for (int sub = 0; sub < pixel_width; ++sub) {
                    const int x = column + sub;
                    if (x >= 0 && x < static_cast<int>(kOutputWidth)) {
                        pixels[static_cast<size_t>(x)] |= plane_bit;
                    }
                }
            }
        }
    }

    // 2. Resolve colours per mode, showing COLOR00 outside the horizontal window.
    const bool dual_playfield = (bplcon0_ & reg::kBplCon0Dblpf) != 0;
    const bool ham = (bplcon0_ & reg::kBplCon0Homod) != 0 && !fetch.hires && fetch.planes >= 5 &&
                     !dual_playfield;
    const bool half_brite = fetch.planes == 6 && !dual_playfield && !ham;
    const bool pf2_front = (bplcon2_ & reg::kBplCon2Pf2Pri) != 0;
    uint16_t ham_color = color_[0];  // HAM hold register

    for (unsigned x = 0; x < kOutputWidth; ++x) {
        const unsigned lowres_h = kViewportStartH + x / 2;
        if (lowres_h < window.hstart || lowres_h >= window.hstop) {
            out[x] = palette_[0];
            ham_color = color_[0];
            continue;
        }
        const unsigned bits = pixels[x];

        if (dual_playfield) {
            // Odd planes (bits 0, 2, 4) form playfield 1, even planes playfield 2.
            const unsigned pf1 = (bits & 1u) | ((bits >> 1) & 2u) | ((bits >> 2) & 4u);
            const unsigned pf2 = ((bits >> 1) & 1u) | ((bits >> 2) & 2u) | ((bits >> 3) & 4u);
            unsigned index = 0;
            if (pf2_front) {
                index = pf2 != 0 ? 8 + pf2 : pf1;
            } else {
                index = pf1 != 0 ? pf1 : (pf2 != 0 ? 8 + pf2 : 0);
            }
            out[x] = palette_[index];
        } else if (ham) {
            // Planes 5-6 select: 00 palette, 01 modify blue, 10 red, 11 green.
            const unsigned data = bits & 0xFu;
            switch ((bits >> 4) & 3u) {
                case 0: ham_color = color_[data]; break;
                case 1: ham_color = static_cast<uint16_t>((ham_color & 0xFF0u) | data); break;
                case 2: ham_color = static_cast<uint16_t>((ham_color & 0x0FFu) | data << 8); break;
                default: ham_color = static_cast<uint16_t>((ham_color & 0xF0Fu) | data << 4); break;
            }
            out[x] = rgb12_to_argb(ham_color);
        } else if (half_brite && (bits & 0x20u) != 0) {
            out[x] = half_brite_palette_[bits & 0x1Fu];
        } else {
            out[x] = palette_[bits & 0x1Fu];
        }
    }
}

}  // namespace amiga
