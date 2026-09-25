#include "core/copper.hpp"

#include "core/custom_registers.hpp"

namespace amiga {

namespace {

// COPxLCH holds A18-A16, COPxLCL A15-A1 (same layout as the bitplane pointers).
uint32_t set_high(uint32_t pointer, uint16_t value) noexcept {
    return ((uint32_t{value} << 16) | (pointer & 0xFFFFu)) & Agnus::kDmaAddressMask;
}

uint32_t set_low(uint32_t pointer, uint16_t value) noexcept {
    return ((pointer & 0xFFFF'0000u) | value) & Agnus::kDmaAddressMask;
}

}  // namespace

bool Copper::write_register(uint16_t offset, uint16_t value) noexcept {
    switch (offset) {
        case reg::kCopCon: copcon_ = value & kCopConDanger; return true;
        case reg::kCop1Lch: cop1lc_ = set_high(cop1lc_, value); return true;
        case reg::kCop1Lcl: cop1lc_ = set_low(cop1lc_, value); return true;
        case reg::kCop2Lch: cop2lc_ = set_high(cop2lc_, value); return true;
        case reg::kCop2Lcl: cop2lc_ = set_low(cop2lc_, value); return true;
        case reg::kCopJmp1: jump(cop1lc_); return true;
        case reg::kCopJmp2: jump(cop2lc_); return true;
        default: return false;
    }
}

bool Copper::beam_reached(uint16_t ir1, uint16_t ir2, uint16_t vpos, uint16_t hpos) noexcept {
    const uint32_t v_mask = 0x80u | ((ir2 >> 8) & 0x7Fu);  // V7 is always compared
    const uint32_t h_mask = ir2 & 0xFEu;
    const uint32_t wait_pos = ((ir1 >> 8) & v_mask) << 8 | (ir1 & h_mask);
    const uint32_t beam_pos = ((vpos & 0xFFu) & v_mask) << 8 | (hpos & 0xFEu & h_mask);
    return beam_pos >= wait_pos;
}

uint16_t Copper::fetch(Agnus::ChipRam chip_ram) noexcept {
    const uint16_t word = static_cast<uint16_t>((chip_ram[pc_] << 8) | chip_ram[pc_ + 1]);
    pc_ = (pc_ + 2) & Agnus::kDmaAddressMask;
    return word;
}

// Registers below $40 are never writable by the Copper; $40-$7E only with CDANG.
bool Copper::move_allowed(uint16_t offset) const noexcept {
    if (offset < 0x40) return false;
    return offset >= 0x80 || danger();
}

void Copper::tick(Agnus::ChipRam chip_ram, uint16_t vpos, uint16_t hpos, bool dma_enabled,
                  CustomChipPort& registers) {
    if (!dma_enabled || (hpos & 1u) != 0) return;

    switch (state_) {
        case State::Stopped:
            return;

        case State::FetchIr1:
            ir1_ = fetch(chip_ram);
            state_ = State::FetchIr2;
            return;

        case State::FetchIr2:
            ir2_ = fetch(chip_ram);
            if ((ir1_ & 1u) == 0) {  // MOVE
                const uint16_t offset = ir1_ & 0x01FEu;
                if (!move_allowed(offset)) {
                    state_ = State::Stopped;
                    return;
                }
                state_ = State::FetchIr1;  // before the write: a COPJMP may redirect us
                registers.write_custom(offset, ir2_);
            } else if ((ir2_ & 1u) == 0) {  // WAIT
                state_ = State::Waiting;
            } else {  // SKIP
                if (beam_reached(ir1_, ir2_, vpos, hpos)) {
                    pc_ = (pc_ + 4) & Agnus::kDmaAddressMask;
                }
                state_ = State::FetchIr1;
            }
            return;

        case State::Waiting:
            if (beam_reached(ir1_, ir2_, vpos, hpos)) state_ = State::Wake;
            return;

        case State::Wake:
            state_ = State::FetchIr1;
            return;
    }
}

}  // namespace amiga
