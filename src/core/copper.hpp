#pragma once

#include <cstdint>

#include "core/agnus.hpp"
#include "core/memory_bus.hpp"

namespace amiga {

// The Copper: Agnus's coprocessor (Amiga Hardware Reference Manual, chapter 2).
//
// It executes a list of 2-word instructions from chip RAM, starting at COP1LC
// at every vertical blank (or at COP1LC/COP2LC when COPJMP1/COPJMP2 is
// strobed), as long as DMACON has DMAEN and COPEN set.
//
//   MOVE  IR1 = 0000000R RRRRRRR0  register offset     IR2 = data
//   WAIT  IR1 = VVVVVVVV HHHHHHH1  beam position       IR2 = BVVVVVVV HHHHHHH0  B = blitter-finished disable, V/H compare enables
//   SKIP  IR1 = VVVVVVVV HHHHHHH1                      IR2 = BVVVVVVV HHHHHHH1
//
// Timing: the Copper only uses even color clocks; each instruction word is
// one fetch. A WAIT that is satisfied costs one extra cycle to wake up.
//
// Limitations: bus contention with bitplane DMA and the CPU is not modelled,
// and there is no blitter yet, so the blitter-finished condition always holds.
class Copper {
public:
    enum class State : uint8_t {
        Stopped,   // at power-on, or after an illegal MOVE; restarts at vertical blank
        FetchIr1,
        FetchIr2,
        Waiting,   // WAIT not satisfied yet
        Wake,      // WAIT satisfied, one cycle before the next fetch
    };

    static constexpr uint16_t kCopConDanger = 1u << 1;  // CDANG

    // Returns false if the register isn't a Copper register.
    bool write_register(uint16_t offset, uint16_t value) noexcept;

    // Start of vertical blank: restart from COP1LC.
    void vertical_blank() noexcept { jump(cop1lc_); }

    // One color clock at beam position (vpos, hpos). MOVEs are performed
    // through `registers`, like any other custom register write.
    void tick(Agnus::ChipRam chip_ram, uint16_t vpos, uint16_t hpos, bool dma_enabled,
              CustomChipPort& registers);

    // WAIT/SKIP comparator: true once the beam is at or past the position,
    // considering only the bits enabled in IR2. V8 is never compared.
    [[nodiscard]] static bool beam_reached(uint16_t ir1, uint16_t ir2, uint16_t vpos,
                                           uint16_t hpos) noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] uint32_t pc() const noexcept { return pc_; }
    [[nodiscard]] uint32_t cop1lc() const noexcept { return cop1lc_; }
    [[nodiscard]] uint32_t cop2lc() const noexcept { return cop2lc_; }
    [[nodiscard]] bool danger() const noexcept { return (copcon_ & kCopConDanger) != 0; }

private:
    void jump(uint32_t address) noexcept {
        pc_ = address;
        state_ = State::FetchIr1;
    }

    [[nodiscard]] uint16_t fetch(Agnus::ChipRam chip_ram) noexcept;
    [[nodiscard]] bool move_allowed(uint16_t offset) const noexcept;

    uint32_t cop1lc_ = 0;
    uint32_t cop2lc_ = 0;
    uint16_t copcon_ = 0;
    uint32_t pc_ = 0;
    uint16_t ir1_ = 0;
    uint16_t ir2_ = 0;
    State state_ = State::Stopped;
};

}  // namespace amiga
