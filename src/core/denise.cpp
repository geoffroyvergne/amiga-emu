#include "core/denise.hpp"

#include <algorithm>

namespace amiga {

namespace {


}  // namespace

void Denise::build_collision_tables() noexcept {
    const unsigned enabled = (clxcon_ >> 6) & 0x3Fu;
    const unsigned match = clxcon_ & 0x3Fu;
    for (unsigned planes = 0; planes < 64; ++planes) {
        const bool odd = ((planes ^ match) & enabled & 0x15u) == 0;   // planes 1, 3, 5
        const bool even = ((planes ^ match) & enabled & 0x2Au) == 0;  // planes 2, 4, 6
        plane_match_[planes] = static_cast<uint8_t>((odd ? 1u : 0u) | (even ? 2u : 0u));
    }
    for (unsigned sprites = 0; sprites < 256; ++sprites) {
        unsigned groups = 0;
        for (unsigned g = 0; g < 4; ++g) {
            const bool odd_enabled = (clxcon_ & (0x1000u << g)) != 0;  // ENSP1/3/5/7
            if ((sprites & (1u << (2 * g))) != 0 || (odd_enabled && (sprites & (2u << (2 * g))) != 0)) groups |= 1u << g;
        }
        sprite_groups_[sprites] = static_cast<uint8_t>(groups);
    }
    // Sprite groups against each other: bits 9-14 = 0/1, 0/2, 0/3, 1/2, 1/3, 2/3.
    for (unsigned groups = 0; groups < 16; ++groups) {
        uint16_t bits = 0;
        unsigned bit = 9;
        for (unsigned a = 0; a < 4; ++a) {
            for (unsigned b = a + 1; b < 4; ++b, ++bit) {
                if ((groups & (1u << a)) != 0 && (groups & (1u << b)) != 0) bits = static_cast<uint16_t>(bits | 1u << bit);
            }
        }
        group_pairs_[groups] = bits;
    }
}

void Denise::render_line(const LineFetch& fetch, const DisplayWindow& window, const Sprites& sprites,
                         Row out) noexcept {
    if (!fetch.vertical_window) {
        std::ranges::fill(out, palette_[0]);  // vertical border
        return;
    }

    // 0. Sprite layer, from each sprite's starts on this line: palette index
    //    (0 = none) and pair per column, drawn from the lowest priority
    //    (sprite 7) up; plus which sprites are opaque there (collisions).
    std::array<uint8_t, kOutputWidth> sprite_color{};
    std::array<uint8_t, kOutputWidth> sprite_pair{};
    std::array<uint8_t, kOutputWidth> sprite_mask{};
    std::array<Sprites::Trigger, Sprites::kMaxTriggers> starts{};
    const bool any_sprite = sprites.active_on_line();
    // Calls f(column, 2-bit value) for each opaque pixel of sprite n.
    const auto for_each_pixel = [&](unsigned n, auto&& f) {
        const size_t count = sprites.triggers(n, starts);
        for (size_t t = 0; t < count; ++t) {
            for (unsigned i = 0; i < 16; ++i) {
                const unsigned bit = 15 - i;
                const unsigned value = ((starts[t].data >> bit) & 1u) | ((starts[t].datb >> bit) & 1u) << 1;
                if (value == 0) continue;
                const int column = 2 * (static_cast<int>(starts[t].hstart) + 1 + static_cast<int>(i) - kViewportStartH);
                for (int c = column; c < column + 2; ++c) {
                    if (c >= 0 && c < static_cast<int>(kOutputWidth)) f(static_cast<size_t>(c), value);
                }
            }
        }
    };
    const auto is_attached = [&](unsigned odd) {
        const size_t count = sprites.triggers(odd, starts);
        for (size_t t = 0; t < count; ++t) {
            if (starts[t].attached) return true;
        }
        return count == 0 && sprites.sprite(odd).attached();
    };
    for (unsigned pair = any_sprite ? 4u : 0u; pair-- > 0;) {
        const unsigned even = 2 * pair;
        const unsigned odd = even + 1;
        if (is_attached(odd)) {  // 4 bits: COLOR16-31
            std::array<uint8_t, kOutputWidth> bits{};
            for_each_pixel(even, [&](size_t c, unsigned v) { bits[c] |= static_cast<uint8_t>(v); sprite_mask[c] |= 1u << even; });
            for_each_pixel(odd, [&](size_t c, unsigned v) { bits[c] |= static_cast<uint8_t>(v << 2); sprite_mask[c] |= 1u << odd; });
            for (size_t c = 0; c < kOutputWidth; ++c) {
                if (bits[c] == 0) continue;
                sprite_color[c] = static_cast<uint8_t>(16u + bits[c]);
                sprite_pair[c] = static_cast<uint8_t>(pair);
            }
            continue;
        }
        for (const unsigned n : {odd, even}) {  // even sprite in front of odd
            for_each_pixel(n, [&](size_t c, unsigned v) {
                sprite_color[c] = static_cast<uint8_t>(16u + 4u * pair + v);
                sprite_pair[c] = static_cast<uint8_t>(pair);
                sprite_mask[c] = static_cast<uint8_t>(sprite_mask[c] | 1u << n);
            });
        }
    }
    const unsigned pf1_priority = bplcon2_ & 7u;
    const unsigned pf2_priority = (bplcon2_ >> 3) & 7u;

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
        // Collisions accumulate until CLXDAT is read: without a sprite here,
        // only bit 0 (planes vs planes) can be set, so skip it once it is.
        if (sprite_mask[x] != 0) {
            clxdat_ |= collisions(bits, sprite_mask[x]);
        } else if ((clxdat_ & 1u) == 0 && plane_match_[bits & 0x3Fu] == 3) {
            clxdat_ |= 1u;
        }

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

        // Sprite in front of the playfield pixel under it? (After the playfield
        // so the HAM hold register still follows the bitplanes.)
        if (sprite_color[x] != 0) {
            unsigned priority = pf2_priority;
            bool playfield_opaque = bits != 0;
            if (dual_playfield) {
                const bool pf1 = (bits & 0x15u) != 0;
                const bool pf2 = (bits & 0x2Au) != 0;
                priority = (pf2 && (!pf1 || pf2_front)) ? pf2_priority : pf1_priority;
            }
            if (!playfield_opaque || sprite_pair[x] < priority) out[x] = palette_[sprite_color[x]];
        }
    }
}

}  // namespace amiga
