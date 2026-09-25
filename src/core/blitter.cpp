#include "core/blitter.hpp"

namespace amiga {

namespace {

// Register offsets (from $DFF000).
constexpr uint16_t kBltCon0 = 0x040, kBltCon1 = 0x042, kBltAfwm = 0x044, kBltAlwm = 0x046;
constexpr uint16_t kBltCpth = 0x048, kBltDptl = 0x056, kBltSize = 0x058;
constexpr uint16_t kBltCmod = 0x060, kBltBmod = 0x062, kBltAmod = 0x064, kBltDmod = 0x066;
constexpr uint16_t kBltCdat = 0x070, kBltBdat = 0x072, kBltAdat = 0x074;

// Barrel shifter: the word shifted by `shift` bits with the bits pushed out of
// the previous word flowing in (from the left ascending, the right descending).
uint16_t barrel(uint16_t previous, uint16_t current, unsigned shift, bool descending) {
    if (shift == 0) return current;
    if (descending) return static_cast<uint16_t>(current << shift | previous >> (16 - shift));
    return static_cast<uint16_t>(current >> shift | previous << (16 - shift));
}

}  // namespace

uint16_t Blitter::minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c) noexcept {
    uint32_t d = 0;
    for (unsigned term = 0; term < 8; ++term) {
        if ((lf & (1u << term)) == 0) continue;
        const uint32_t ta = (term & 4u) != 0 ? a : ~uint32_t{a};
        const uint32_t tb = (term & 2u) != 0 ? b : ~uint32_t{b};
        const uint32_t tc = (term & 1u) != 0 ? c : ~uint32_t{c};
        d |= ta & tb & tc;
    }
    return static_cast<uint16_t>(d);
}

bool Blitter::write_register(uint16_t offset, uint16_t value, bool& started) noexcept {
    started = false;
    // Pointers: BLTCPT $048, BLTBPT $04C, BLTAPT $050, BLTDPT $054 (high word, low word).
    if (offset >= kBltCpth && offset <= kBltDptl) {
        static constexpr std::array<unsigned, 4> kOrder = {kC, kB, kA, kD};
        uint32_t& pointer = pt_[kOrder[(offset - kBltCpth) / 4]];
        if ((offset & 2u) == 0) {
            pointer = ((uint32_t{value} << 16) | (pointer & 0xFFFFu)) & kAddressMask;
        } else {
            pointer = ((pointer & 0xFFFF'0000u) | value) & kAddressMask;
        }
        return true;
    }
    switch (offset) {
        case kBltCon0: bltcon0_ = value; return true;
        case kBltCon1: bltcon1_ = value; return true;
        case kBltAfwm: afwm_ = value; return true;
        case kBltAlwm: alwm_ = value; return true;
        case kBltAmod: mod_[kA] = static_cast<int16_t>(value & 0xFFFEu); return true;
        case kBltBmod: mod_[kB] = static_cast<int16_t>(value & 0xFFFEu); return true;
        case kBltCmod: mod_[kC] = static_cast<int16_t>(value & 0xFFFEu); return true;
        case kBltDmod: mod_[kD] = static_cast<int16_t>(value & 0xFFFEu); return true;
        case kBltAdat: data_[kA] = value; return true;
        case kBltBdat: data_[kB] = value; return true;
        case kBltCdat: data_[kC] = value; return true;
        case kBltSize: {
            // H9-H0 | W5-W0; 0 means the maximum (1024 rows, 64 words).
            const unsigned height = value >> 6;
            const unsigned width = value & 0x3Fu;
            height_ = static_cast<uint16_t>(height == 0 ? 1024 : height);
            width_ = static_cast<uint16_t>(width == 0 ? 64 : width);
            pending_ = true;
            started = true;
            return true;
        }
        default: return false;
    }
}

uint16_t Blitter::read(ChipRam chip_ram, uint32_t address) noexcept {
    address &= kAddressMask;
    return static_cast<uint16_t>(chip_ram[address] << 8 | chip_ram[address + 1]);
}

void Blitter::write(ChipRam chip_ram, uint32_t address, uint16_t value) noexcept {
    address &= kAddressMask;
    chip_ram[address] = static_cast<uint8_t>(value >> 8);
    chip_ram[address + 1] = static_cast<uint8_t>(value);
}

void Blitter::run(ChipRam chip_ram) noexcept {
    pending_ = false;
    if ((bltcon1_ & kLine) != 0) {
        run_line(chip_ram);
    } else {
        run_area(chip_ram);
    }
}

void Blitter::run_area(ChipRam chip_ram) noexcept {
    const bool descending = (bltcon1_ & kDesc) != 0;
    const bool fill = descending && (bltcon1_ & (kIfe | kEfe)) != 0;
    const bool exclusive = (bltcon1_ & kEfe) != 0;
    const unsigned a_shift = bltcon0_ >> 12;
    const unsigned b_shift = bltcon1_ >> 12;
    const auto lf = static_cast<uint8_t>(bltcon0_);
    const auto step = static_cast<uint32_t>(descending ? -2 : 2);

    uint16_t a_previous = 0;
    uint16_t b_previous = 0;
    bool any_set = false;

    for (unsigned row = 0; row < height_; ++row) {
        bool fill_carry = (bltcon1_ & kFci) != 0;
        for (unsigned word = 0; word < width_; ++word) {
            uint16_t a = data_[kA];
            if ((bltcon0_ & kUseA) != 0) {
                a = read(chip_ram, pt_[kA]);
                pt_[kA] += step;
                data_[kA] = a;
            }
            if (word == 0) a &= afwm_;
            if (word == width_ - 1u) a &= alwm_;
            const uint16_t a_shifted = barrel(a_previous, a, a_shift, descending);
            a_previous = a;

            uint16_t b_shifted = data_[kB];
            if ((bltcon0_ & kUseB) != 0) {
                const uint16_t b = read(chip_ram, pt_[kB]);
                pt_[kB] += step;
                b_shifted = barrel(b_previous, b, b_shift, descending);
                b_previous = b;
            }

            uint16_t c = data_[kC];
            if ((bltcon0_ & kUseC) != 0) {
                c = read(chip_ram, pt_[kC]);
                pt_[kC] += step;
                data_[kC] = c;
            }

            uint16_t d = minterm(lf, a_shifted, b_shifted, c);
            if (fill) {
                // From the rightmost pixel (bit 0) leftwards; each set bit toggles the fill.
                uint16_t filled = 0;
                for (unsigned bit = 0; bit < 16; ++bit) {
                    const bool edge = (d & (1u << bit)) != 0;
                    if (edge) fill_carry = !fill_carry;
                    const bool out = exclusive ? fill_carry : (fill_carry || edge);
                    if (out) filled = static_cast<uint16_t>(filled | 1u << bit);
                }
                d = filled;
            }
            if (d != 0) any_set = true;
            d_data_ = d;
            if ((bltcon0_ & kUseD) != 0) {
                write(chip_ram, pt_[kD], d);
                pt_[kD] += step;
            }
        }
        // Modulos: added ascending, subtracted descending (enabled channels only).
        for (const unsigned channel : {kA, kB, kC, kD}) {
            static constexpr std::array<uint16_t, 4> kUse = {kUseA, kUseB, kUseC, kUseD};
            if ((bltcon0_ & kUse[channel]) == 0) continue;
            const auto modulo = static_cast<uint32_t>(static_cast<int32_t>(mod_[channel]));
            pt_[channel] = descending ? pt_[channel] - modulo : pt_[channel] + modulo;
        }
    }
    for (uint32_t& pointer : pt_) pointer &= kAddressMask;
    zero_ = !any_set;
}

// Line mode (HRM "Line Drawing"): BLTAPT holds the error term, BLTAMOD and
// BLTBMOD its increments (4*(dy-dx) and 4*dy), BLTCPT/BLTDPT the address of
// the first pixel's word and BLTCMOD the row size. ASH is the pixel within
// the word, BSH the starting texture bit, BLTSIZE height the pixel count.
void Blitter::run_line(ChipRam chip_ram) noexcept {
    const auto lf = static_cast<uint8_t>(bltcon0_);
    const bool single_dot = (bltcon1_ & kSing) != 0;
    unsigned pixel_shift = bltcon0_ >> 12;
    const unsigned texture_shift = bltcon1_ >> 12;
    uint16_t texture = static_cast<uint16_t>(data_[kB] >> texture_shift | data_[kB] << ((16 - texture_shift) & 15u));
    auto error = static_cast<int16_t>(pt_[kA]);
    bool sign = (bltcon1_ & kSign) != 0;
    uint32_t address = pt_[kC];
    uint32_t d_address = pt_[kD];
    bool dot_on_row = false;
    bool any_set = false;
    const auto row = static_cast<uint32_t>(static_cast<int32_t>(mod_[kC]));

    const auto step_x = [&](bool left) {
        if (left) {
            if (pixel_shift-- == 0) {
                pixel_shift = 15;
                address -= 2;
            }
        } else if (++pixel_shift == 16) {
            pixel_shift = 0;
            address += 2;
        }
    };
    const auto step_y = [&](bool up) {
        address = up ? address - row : address + row;
        dot_on_row = false;
    };

    for (unsigned pixel = 0; pixel < height_; ++pixel) {
        const uint16_t a = static_cast<uint16_t>((data_[kA] & afwm_) >> pixel_shift);
        const uint16_t b = (texture & 1u) != 0 ? 0xFFFF : 0x0000;
        const uint16_t c = read(chip_ram, address);
        uint16_t d = minterm(lf, a, b, c);
        if (single_dot && dot_on_row) d = c;
        dot_on_row = true;
        if (d != 0) any_set = true;
        d_data_ = d;
        write(chip_ram, d_address, d);

        // Error term, then the "sometimes" step (only when the error was not
        // negative) and the "always" step. SUD: the sometimes step is up/down
        // (so x is stepped always); SUL/AUL: that step goes up or left.
        const bool sometimes_vertical = (bltcon1_ & kSud) != 0;
        if (!sign) {
            error = static_cast<int16_t>(error + mod_[kA]);
            if (sometimes_vertical) {
                step_y((bltcon1_ & kSul) != 0);
            } else {
                step_x((bltcon1_ & kSul) != 0);
            }
        } else {
            error = static_cast<int16_t>(error + mod_[kB]);
        }
        if (sometimes_vertical) {
            step_x((bltcon1_ & kAul) != 0);
        } else {
            step_y((bltcon1_ & kAul) != 0);
        }
        sign = error < 0;
        texture = static_cast<uint16_t>(texture << 1 | texture >> 15);
        address &= kAddressMask;
        d_address = address;
    }
    pt_[kA] = (pt_[kA] & 0xFFFF'0000u) | static_cast<uint16_t>(error);
    pt_[kC] = address;
    pt_[kD] = d_address;
    bltcon0_ = static_cast<uint16_t>((bltcon0_ & 0x0FFFu) | pixel_shift << 12);
    bltcon1_ = static_cast<uint16_t>((bltcon1_ & ~kSign) | (sign ? kSign : 0));
    zero_ = !any_set;
}

}  // namespace amiga
