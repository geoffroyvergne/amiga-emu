#pragma once

#include <cstdint>
#include <cstdio>
#include <span>

#include "core/custom_registers.hpp"
#include "core/memory_bus.hpp"

namespace amiga {

// Paula's disk controller: MFM data from the selected drive to chip RAM by
// DMA (Amiga Hardware Reference Manual, chapter 8).
//
//   DSKPTH/DSKPTL  DMA pointer
//   DSKLEN         bit 15 DMAEN, bit 14 WRITE, bits 13-0 length in words.
//                  A transfer starts on the second write with DMAEN set
//                  (a safety measure); a write with DMAEN clear stops it.
//   DSKSYNC        sync word; with ADKCON WORDSYNC set, a read waits for it
//                  (the sync word itself is not stored)
// The data arrives as a bit stream: a 16-bit shift register is compared with
// DSKSYNC after every bit, so a sync pattern is found at any bit position,
// not only on word boundaries. With WORDSYNC, a match re-aligns the word
// boundary to just after the sync (loaders rely on unaligned syncs).
//   DSKBYTR        byte-ready / DMA-on / write / word-equal status + last byte
// Interrupts: DSKSYN (level 5) each time the sync word passes, DSKBLK
// (level 1) when a transfer completes. DMACON DSKEN must be on.
//
// Writing to disk is not emulated: a write transfer completes at once (with
// DSKBLK) and its data is discarded.
class DiskController {
public:
    static constexpr uint16_t kDskPth = 0x020, kDskPtl = 0x022, kDskLen = 0x024, kDskSync = 0x07E;
    static constexpr uint16_t kDskBytR = 0x01A, kAdkConR = 0x010, kAdkCon = 0x09E;
    static constexpr uint16_t kDmaEnBit = 0x8000, kWriteBit = 0x4000;
    static constexpr uint16_t kAdkWordSync = 1u << 10;
    static constexpr uint32_t kAddressMask = (MemoryBus::kChipRamSize - 1) & ~1u;

    using ChipRam = std::span<uint8_t, MemoryBus::kChipRamSize>;

    // Returns false if the register isn't a disk controller register. May
    // complete a (write) transfer at once: interrupt bits go to `requests`.
    bool write_register(uint16_t offset, uint16_t value, uint16_t& requests) noexcept {
        switch (offset) {
            case kDskPth: pointer_ = ((uint32_t{value} << 16) | (pointer_ & 0xFFFFu)) & kAddressMask; return true;
            case kDskPtl: pointer_ = ((pointer_ & 0xFFFF'0000u) | value) & kAddressMask; return true;
            case kDskSync: sync_ = value; return true;
            case kAdkCon: {
                const uint16_t bits = value & 0x7FFFu;
                adkcon_ = (value & reg::kSetClr) != 0 ? static_cast<uint16_t>(adkcon_ | bits)
                                                      : static_cast<uint16_t>(adkcon_ & ~bits);
                return true;
            }
            case kDskLen: write_dsklen(value, requests); return true;
            default: return false;
        }
    }

    bool read_register(uint16_t offset, uint16_t& value, bool dma_enabled) noexcept {
        switch (offset) {
            case kAdkConR: value = adkcon_; return true;
            case kDskBytR:
                value = static_cast<uint16_t>((byte_ready_ ? 0x8000u : 0u) | (active_ && dma_enabled ? 0x4000u : 0u) |
                                              (word_equal_ ? 0x1000u : 0u) | (last_word_ & 0xFFu));
                byte_ready_ = false;
                return true;
            default: return false;
        }
    }

    // 16 bits read by the selected drive's head, most significant first.
    // Interrupt bits go to `requests`.
    void disk_word(uint16_t word, ChipRam chip_ram, bool dma_enabled, uint16_t& requests) noexcept {
        const bool word_sync = (adkcon_ & kAdkWordSync) != 0;
        for (int bit = 15; bit >= 0; --bit) {
            shift_ = static_cast<uint16_t>(uint32_t{shift_} << 1 | ((uint32_t{word} >> bit) & 1u));
            ++bits_;
            const bool sync = shift_ == sync_;
            word_equal_ = sync;
            if (sync) {
                requests |= reg::kIntDskSyn;
                if (word_sync && active_ && waiting_for_sync_) {
                    waiting_for_sync_ = false;  // not stored; words start after it
                    bits_ = 0;
                    continue;
                }
                if (word_sync) bits_ = 16;  // a sync match is a word boundary
            }
            if (bits_ < 16) continue;
            bits_ = 0;
            store(shift_, chip_ram, dma_enabled, requests);
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] uint32_t pointer() const noexcept { return pointer_; }
    [[nodiscard]] uint16_t adkcon() const noexcept { return adkcon_; }

private:
    // A complete word: DSKBYTR sees it; DMA stores it once the transfer runs.
    void store(uint16_t word, ChipRam chip_ram, bool dma_enabled, uint16_t& requests) noexcept {
        last_word_ = word;
        byte_ready_ = true;
        if (!active_ || !dma_enabled || waiting_for_sync_) return;
        chip_ram[pointer_] = static_cast<uint8_t>(word >> 8);
        chip_ram[pointer_ + 1] = static_cast<uint8_t>(word);
        pointer_ = (pointer_ + 2) & kAddressMask;
        if (--remaining_ == 0) {
            active_ = false;
            requests |= reg::kIntDskBlk;
        }
    }

    void write_dsklen(uint16_t value, uint16_t& requests) noexcept {
        if ((value & kDmaEnBit) == 0) {
            active_ = false;
            armed_ = false;
            return;
        }
        if (!armed_) {  // first write: arm only
            armed_ = true;
            return;
        }
        armed_ = false;
        remaining_ = value & 0x3FFFu;
        if ((value & kWriteBit) != 0 || remaining_ == 0) {
            if ((value & kWriteBit) != 0 && !warned_write_) {
                warned_write_ = true;
                std::fprintf(stderr, "[disk] warning: disk writes are not emulated (data discarded)\n");
            }
            active_ = false;
            requests |= reg::kIntDskBlk;
            return;
        }
        active_ = true;
        waiting_for_sync_ = (adkcon_ & kAdkWordSync) != 0;
    }

    uint32_t pointer_ = 0;
    uint16_t sync_ = 0x4489;
    uint16_t adkcon_ = 0;
    uint16_t remaining_ = 0;
    uint16_t last_word_ = 0;
    uint16_t shift_ = 0;  // incoming bits
    unsigned bits_ = 0;   // bits since the last word boundary
    bool armed_ = false;
    bool active_ = false;
    bool waiting_for_sync_ = false;
    bool byte_ready_ = false;
    bool word_equal_ = false;
    bool warned_write_ = false;
};

}  // namespace amiga
