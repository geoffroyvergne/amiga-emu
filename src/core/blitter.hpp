#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/memory_bus.hpp"

namespace amiga {

// The OCS blitter (Amiga Hardware Reference Manual, chapter 6).
//
// Area mode: sources A, B, C and destination D, any of the 256 minterms,
// barrel shifts on A and B, first/last word masks on A, modulos, ascending
// or descending, and inclusive/exclusive area fill (descending mode).
// Line mode: Bresenham lines with octant control, texture (B) and the
// one-dot-per-line option.
//
// Timing: a blit runs to completion when it starts (on the BLTSIZE write, or
// when blitter DMA gets enabled); BBUSY is therefore never seen set. Bus
// cycles are not taken from the CPU yet.
class Blitter {
public:
    static constexpr uint32_t kAddressMask = (MemoryBus::kChipRamSize - 1) & ~1u;

    // BLTCON0 bits.
    static constexpr uint16_t kUseA = 1u << 11;
    static constexpr uint16_t kUseB = 1u << 10;
    static constexpr uint16_t kUseC = 1u << 9;
    static constexpr uint16_t kUseD = 1u << 8;
    // BLTCON1 bits (area mode).
    static constexpr uint16_t kLine = 1u << 0;
    static constexpr uint16_t kDesc = 1u << 1;
    static constexpr uint16_t kFci = 1u << 2;
    static constexpr uint16_t kIfe = 1u << 3;
    static constexpr uint16_t kEfe = 1u << 4;
    // BLTCON1 bits (line mode).
    static constexpr uint16_t kSing = 1u << 1;
    static constexpr uint16_t kAul = 1u << 2;
    static constexpr uint16_t kSul = 1u << 3;
    static constexpr uint16_t kSud = 1u << 4;
    static constexpr uint16_t kSign = 1u << 6;

    using ChipRam = std::span<uint8_t, MemoryBus::kChipRamSize>;

    // Returns false if the register isn't a blitter register. A BLTSIZE
    // write sets `started`.
    bool write_register(uint16_t offset, uint16_t value, bool& started) noexcept;

    // Runs the blit last started with BLTSIZE.
    void run(ChipRam chip_ram) noexcept;

    [[nodiscard]] bool pending() const noexcept { return pending_; }
    void set_pending(bool pending) noexcept { pending_ = pending; }
    [[nodiscard]] bool zero() const noexcept { return zero_; }  // DMACONR BZERO
    [[nodiscard]] uint16_t last_d() const noexcept { return d_data_; }  // BLTDDAT
    [[nodiscard]] uint32_t pointer(unsigned channel) const noexcept { return pt_[channel]; }  // 0=A..3=D

    // Minterm function: for each bit, LF bit index (a<<2 | b<<1 | c).
    [[nodiscard]] static uint16_t minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c) noexcept;

private:
    enum Channel : unsigned { kA = 0, kB = 1, kC = 2, kD = 3 };

    void run_area(ChipRam chip_ram) noexcept;
    void run_line(ChipRam chip_ram) noexcept;
    static uint16_t read(ChipRam chip_ram, uint32_t address) noexcept;
    static void write(ChipRam chip_ram, uint32_t address, uint16_t value) noexcept;

    uint16_t bltcon0_ = 0;
    uint16_t bltcon1_ = 0;
    uint16_t afwm_ = 0xFFFF;
    uint16_t alwm_ = 0xFFFF;
    std::array<uint32_t, 4> pt_{};    // A, B, C, D pointers
    std::array<int16_t, 4> mod_{};    // A, B, C, D modulos
    std::array<uint16_t, 3> data_{};  // A, B, C data registers
    uint16_t width_ = 0;              // words
    uint16_t height_ = 0;             // rows
    uint16_t d_data_ = 0;
    bool pending_ = false;
    bool zero_ = false;
};

}  // namespace amiga
