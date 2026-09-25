#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/custom_registers.hpp"
#include "core/memory_bus.hpp"
#include "core/timing.hpp"

namespace amiga {

// Display window (DIWSTRT/DIWSTOP) decoded to full positions: horizontal in
// lowres pixels, vertical in lines. Stop positions are exclusive.
struct DisplayWindow {
    uint16_t hstart = 0;
    uint16_t hstop = 0;
    uint16_t vstart = 0;
    uint16_t vstop = 0;
};

// Bitplane data fetched by Agnus for one line, handed to Denise.
struct LineFetch {
    static constexpr unsigned kMaxPlanes = 6;
    static constexpr unsigned kMaxWords = 50;  // hires, DDFSTRT $18 to DDFSTOP $D8

    bool vertical_window = false;  // line inside DIWSTRT/DIWSTOP vertically
    bool hires = false;
    unsigned planes = 0;           // 0 = no bitplane DMA on this line
    unsigned words = 0;            // per plane
    uint16_t first_pixel = 0;      // lowres position of the first fetched pixel (no scroll)
    std::array<std::array<uint16_t, kMaxWords>, kMaxPlanes> data{};
};

// Agnus (OCS 8371, PAL) — beam counter, DMACON, display window and bitplane
// DMA. The Copper, also part of Agnus, is modelled separately in copper.hpp.
//
// DMA reads chip RAM directly: it never sees the ROM overlay or other CPU
// address space. OCS Agnus drives A1-A18, so pointers cover the 512KB of
// chip RAM, always at even addresses.
//
// Bitplane fetching is done a whole line at a time (fetch_line), not slot by
// slot, so DMA cycles are not taken from the Copper or the CPU yet.
class Agnus {
public:
    static constexpr uint32_t kDmaAddressMask = (MemoryBus::kChipRamSize - 1) & ~1u;  // $7FFFE
    static constexpr uint16_t kDmaConBits = 0x07FF;  // writable DMACON bits
    static constexpr uint16_t kVPosRLof = 0x8000;    // long frame (always, non-interlaced)
    static constexpr uint16_t kAgnusIdPalOcs = 0x00;  // VPOSR bits 14-8
    static constexpr uint16_t kDdfMask = 0x00FC;      // DDFSTRT/DDFSTOP bits used by OCS
    static constexpr uint16_t kDdfMin = 0x18;         // earliest fetch start (hardware limit)
    static constexpr uint16_t kDdfMax = 0xD8;         // latest fetch stop (hardware limit)

    using ChipRam = std::span<const uint8_t, MemoryBus::kChipRamSize>;

    // Returns false if the register doesn't belong to Agnus (or isn't emulated).
    bool write_register(uint16_t offset, uint16_t value) noexcept {
        if (offset >= reg::kBpl1Pth && offset <= reg::kBpl6Ptl) {
            uint32_t& pointer = bplpt_[(offset - reg::kBpl1Pth) / 4u];
            if ((offset & 2u) == 0) {  // BPLxPTH
                pointer = ((uint32_t{value} << 16) | (pointer & 0xFFFFu)) & kDmaAddressMask;
            } else {  // BPLxPTL
                pointer = ((pointer & 0xFFFF'0000u) | value) & kDmaAddressMask;
            }
            return true;
        }
        switch (offset) {
            case reg::kDmaCon: {
                const uint16_t bits = value & kDmaConBits;
                if ((value & reg::kSetClr) != 0) {
                    dmacon_ |= bits;
                } else {
                    dmacon_ &= static_cast<uint16_t>(~bits);
                }
                return true;
            }
            case reg::kBplCon0: bplcon0_ = value; return true;
            case reg::kBpl1Mod: bplmod_[0] = static_cast<int16_t>(value & 0xFFFEu); return true;
            case reg::kBpl2Mod: bplmod_[1] = static_cast<int16_t>(value & 0xFFFEu); return true;
            case reg::kDiwStrt: diwstrt_ = value; return true;
            case reg::kDiwStop: diwstop_ = value; return true;
            case reg::kDdfStrt: ddfstrt_ = value & kDdfMask; return true;
            case reg::kDdfStop: ddfstop_ = value & kDdfMask; return true;
            default: return false;
        }
    }

    // Returns false if the register isn't a readable Agnus register.
    bool read_register(uint16_t offset, uint16_t& value) const noexcept {
        switch (offset) {
            case reg::kDmaConR:
                value = dmacon_;  // BBUSY/BZERO stay 0: no blitter yet
                return true;
            case reg::kVPosR:
                value = static_cast<uint16_t>(kVPosRLof | kAgnusIdPalOcs << 8 | ((vpos_ >> 8) & 1u));
                return true;
            case reg::kVHPosR:
                value = static_cast<uint16_t>((vpos_ & 0xFFu) << 8 | hpos_);
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] uint16_t dmacon() const noexcept { return dmacon_; }
    [[nodiscard]] bool dma_enabled(uint16_t channel) const noexcept {
        return (dmacon_ & reg::kDmaEn) != 0 && (dmacon_ & channel) != 0;
    }

    // --- Beam counter -----------------------------------------------------------

    [[nodiscard]] uint16_t vpos() const noexcept { return vpos_; }
    [[nodiscard]] uint16_t hpos() const noexcept { return hpos_; }  // color clocks, $00-$E2

    // Advances one color clock. Returns true when a new frame starts (0,0).
    bool advance_beam() noexcept {
        if (++hpos_ < timing::kPalColorClocksPerLine) return false;
        hpos_ = 0;
        if (++vpos_ < timing::kPalLinesPerFrame) return false;
        vpos_ = 0;
        return true;
    }

    // --- Display window and data fetch ------------------------------------------

    // DIWSTRT: V8 = 0, H8 = 0. DIWSTOP: V8 = !V7, H8 = 1.
    [[nodiscard]] DisplayWindow window() const noexcept {
        const auto stop_v8 = static_cast<uint16_t>((diwstop_ & 0x8000u) != 0 ? 0 : 0x100);
        return {
            .hstart = static_cast<uint16_t>(diwstrt_ & 0xFFu),
            .hstop = static_cast<uint16_t>(0x100u | (diwstop_ & 0xFFu)),
            .vstart = static_cast<uint16_t>(diwstrt_ >> 8),
            .vstop = static_cast<uint16_t>(stop_v8 | (diwstop_ >> 8)),
        };
    }

    [[nodiscard]] bool hires() const noexcept { return (bplcon0_ & reg::kBplCon0Hires) != 0; }

    // Bitplanes Agnus actually fetches: BPU, except that OCS has DMA slots for
    // at most 4 hires planes, and BPU = 7 is not a valid setting (treated as 0).
    [[nodiscard]] unsigned fetched_planes() const noexcept {
        const unsigned bpu = (bplcon0_ >> reg::kBplCon0BpuShift) & 7u;
        if (bpu > 6) return 0;
        return hires() && bpu > 4 ? 4 : bpu;
    }

    // Words fetched per plane and line. OCS fetches bitplanes in 8-color-clock
    // units from DDFSTRT, in both resolutions (1 word per plane per unit in
    // lowres, 2 in hires), up to and including the unit DDFSTOP falls in.
    // So hires $3C-$D0 (Workbench 1.3) and $3C-$D4 (the HRM's example) both
    // fetch 40 words, and lowres $38-$D0 fetches 20.
    [[nodiscard]] unsigned fetch_words() const noexcept {
        const unsigned start = ddfstrt_ < kDdfMin ? kDdfMin : ddfstrt_;
        const unsigned stop = ddfstop_ > kDdfMax ? kDdfMax : ddfstop_;
        if (stop < start) return 0;
        const unsigned units = (stop - start + 7) / 8 + 1;
        return hires() ? 2 * units : units;
    }

    // Lowres position of the first fetched pixel: the fetch pipeline delays
    // output by 17 lowres pixels (9 in hires), so DDFSTRT $38 lines up with
    // DIWSTRT H $81 in lowres and DDFSTRT $3C does in hires.
    [[nodiscard]] uint16_t first_pixel() const noexcept {
        const unsigned start = ddfstrt_ < kDdfMin ? kDdfMin : ddfstrt_;
        return static_cast<uint16_t>(2 * start + (hires() ? 9 : 17));
    }

    // Bitplane DMA for one line: planes 1..n, each fetch_words() words from
    // BPLxPT, then the modulo is added (BPL1MOD odd planes, BPL2MOD even).
    // DMA only runs inside the vertical display window with BPLEN set.
    void fetch_line(ChipRam chip_ram, uint16_t vpos, LineFetch& out) noexcept {
        const DisplayWindow diw = window();
        out.vertical_window = vpos >= diw.vstart && vpos < diw.vstop;
        out.hires = hires();
        out.planes = out.vertical_window && dma_enabled(reg::kBplEn) ? fetched_planes() : 0;
        out.words = fetch_words();
        out.first_pixel = first_pixel();

        for (unsigned plane = 0; plane < out.planes; ++plane) {
            uint32_t& pointer = bplpt_[plane];
            for (unsigned word = 0; word < out.words; ++word) {
                out.data[plane][word] =
                    static_cast<uint16_t>((chip_ram[pointer] << 8) | chip_ram[pointer + 1]);
                pointer = (pointer + 2) & kDmaAddressMask;
            }
            const int16_t modulo = bplmod_[plane & 1u];
            pointer = static_cast<uint32_t>(static_cast<int32_t>(pointer) + modulo) & kDmaAddressMask;
        }
    }

    [[nodiscard]] uint32_t bplpt(unsigned plane) const noexcept { return bplpt_[plane]; }
    [[nodiscard]] int16_t bplmod(unsigned plane) const noexcept { return bplmod_[plane & 1u]; }

private:
    uint16_t dmacon_ = 0;
    uint16_t vpos_ = 0;
    uint16_t hpos_ = 0;
    uint16_t bplcon0_ = 0;
    uint16_t diwstrt_ = 0;
    uint16_t diwstop_ = 0;
    uint16_t ddfstrt_ = 0;
    uint16_t ddfstop_ = 0;
    std::array<uint32_t, LineFetch::kMaxPlanes> bplpt_{};
    std::array<int16_t, 2> bplmod_{};  // [0] BPL1MOD (odd planes), [1] BPL2MOD (even planes)
};

}  // namespace amiga
