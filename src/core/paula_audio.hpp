#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/custom_registers.hpp"
#include "core/memory_bus.hpp"

namespace amiga {

// Paula's four audio channels (Amiga Hardware Reference Manual, chapter 5).
//
// Per channel x, registers at $0A0 + $10 * x:
//   AUDxLCH/LCL  +0/+2  sample start (latched when a buffer starts)
//   AUDxLEN      +4     buffer length in words (0 = 65536)
//   AUDxPER      +6     color clocks per sample
//   AUDxVOL      +8     volume 0-64
//   AUDxDAT      +A     two samples, written by DMA or by the CPU
// With DMA on (DMACON DMAEN + AUDxEN) the channel plays the buffer over and
// over: each word holds two signed 8-bit samples (high byte first), each
// lasting PER color clocks. The AUDx interrupt fires each time a buffer's
// location and length are latched (at the start and on every repeat), so
// software can queue the next buffer. With DMA off, writing AUDxDAT plays
// that word and raises the interrupt when it is done ("manual" mode).
//
// Stereo: channels 0 and 3 on the left, 1 and 2 on the right.
// Not emulated: attached channels (ADKCON USEx), the audio low-pass filter
// (CIA-A PA1) and DMA cycle timing.
class PaulaAudio {
public:
    static constexpr uint16_t kBase = 0x0A0;
    static constexpr uint32_t kAddressMask = (MemoryBus::kChipRamSize - 1) & ~1u;
    static constexpr uint16_t kAud0En = 1u << 0;  // DMACON AUD0EN; AUD1EN-AUD3EN follow

    using ChipRam = std::span<const uint8_t, MemoryBus::kChipRamSize>;

    bool write_register(uint16_t offset, uint16_t value) noexcept {
        if (offset < kBase || offset >= kBase + 0x40) return false;
        Channel& ch = channels_[(offset - kBase) / 0x10];
        switch (offset & 0xFu) {
            case 0x0: ch.location = ((uint32_t{value} << 16) | (ch.location & 0xFFFFu)) & kAddressMask; return true;
            case 0x2: ch.location = ((ch.location & 0xFFFF'0000u) | value) & kAddressMask; return true;
            case 0x4: ch.length = value; return true;
            case 0x6: ch.period = value; return true;
            case 0x8: ch.volume = static_cast<uint8_t>((value & 0x40u) != 0 ? 64 : (value & 0x3Fu)); return true;
            case 0xA:
                ch.data = value;
                if (!ch.dma) {  // manual mode: play this word
                    idle_ = false;
                    ch.manual = true;
                    ch.low_byte = false;
                    ch.countdown = period_of(ch);
                    ch.output = static_cast<int8_t>(value >> 8);
                }
                return true;
            default: return false;
        }
    }

    // One color clock. Interrupt bits (AUD0-AUD3) go to `requests`.
    void tick(ChipRam chip_ram, uint16_t dmacon, uint16_t& requests) noexcept {
        const bool master = (dmacon & reg::kDmaEn) != 0;
        // Fast path: nothing playing and no audio DMA to start.
        if (idle_ && (!master || (dmacon & 0xFu) == 0)) return;
        idle_ = false;
        for (unsigned x = 0; x < 4; ++x) {
            Channel& ch = channels_[x];
            const auto interrupt = static_cast<uint16_t>(reg::kIntAud0 << x);
            const bool dma = master && (dmacon & (kAud0En << x)) != 0;

            if (dma && !ch.dma) {  // DMA switched on: latch the buffer and fetch
                ch.dma = true;
                ch.manual = false;
                start_buffer(ch, requests, interrupt);
                fetch(ch, chip_ram, requests, interrupt);
                continue;
            }
            if (!dma && ch.dma) {  // switched off: silence
                ch.dma = false;
                ch.output = 0;
                continue;
            }
            if (!ch.dma && !ch.manual) continue;

            if (--ch.countdown != 0) continue;
            ch.countdown = period_of(ch);
            if (!ch.low_byte) {
                ch.low_byte = true;
                ch.output = static_cast<int8_t>(ch.data & 0xFFu);
            } else if (ch.dma) {
                fetch(ch, chip_ram, requests, interrupt);
            } else {  // manual word done: interrupt, then repeat it until rewritten
                requests |= interrupt;
                ch.low_byte = false;
                ch.output = static_cast<int8_t>(ch.data >> 8);
            }
        }
        idle_ = true;
        for (const Channel& ch : channels_) {
            if (ch.dma || ch.manual || ch.output != 0) idle_ = false;
        }
    }

    // True while all channels are silent and stopped (output stays 0).
    [[nodiscard]] bool idle() const noexcept { return idle_; }

    // Current output, scaled by volume: each channel -8192..8128.
    [[nodiscard]] int channel_output(unsigned x) const noexcept {
        return channels_[x].output * channels_[x].volume;
    }
    [[nodiscard]] int left() const noexcept { return channel_output(0) + channel_output(3); }
    [[nodiscard]] int right() const noexcept { return channel_output(1) + channel_output(2); }

private:
    struct Channel {
        uint32_t location = 0;  // AUDxLC (latch)
        uint16_t length = 0;    // AUDxLEN (latch)
        uint16_t period = 0;
        uint8_t volume = 0;
        uint16_t data = 0;      // the word being played
        uint32_t pointer = 0;   // DMA counters
        uint32_t remaining = 0;
        uint32_t countdown = 1;
        int8_t output = 0;
        bool low_byte = false;
        bool dma = false;
        bool manual = false;
    };

    static uint32_t period_of(const Channel& ch) noexcept { return ch.period == 0 ? 65536u : ch.period; }

    static void start_buffer(Channel& ch, uint16_t& requests, uint16_t interrupt) noexcept {
        ch.pointer = ch.location;
        ch.remaining = ch.length == 0 ? 65536u : ch.length;
        requests |= interrupt;  // location/length latched: software may queue the next buffer
    }

    static void fetch(Channel& ch, ChipRam chip_ram, uint16_t& requests, uint16_t interrupt) noexcept {
        ch.data = static_cast<uint16_t>(chip_ram[ch.pointer] << 8 | chip_ram[ch.pointer + 1]);
        ch.pointer = (ch.pointer + 2) & kAddressMask;
        if (--ch.remaining == 0) start_buffer(ch, requests, interrupt);
        ch.low_byte = false;
        ch.countdown = period_of(ch);
        ch.output = static_cast<int8_t>(ch.data >> 8);
    }

    std::array<Channel, 4> channels_{};
    bool idle_ = true;
};

}  // namespace amiga
