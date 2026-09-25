#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/memory_bus.hpp"

namespace amiga {

// The eight hardware sprites (Amiga Hardware Reference Manual, chapter 4):
// the DMA channels in Agnus and the sprite registers/serialisers in Denise.
//
// Registers, sprite x:
//   SPRxPTH/PTL  $120 + 4x   DMA pointer
//   SPRxPOS      $140 + 8x   bits 15-8 VSTART V7-V0, bits 7-0 HSTART H8-H1
//   SPRxCTL      $142 + 8x   bits 15-8 VSTOP V7-V0, bit 7 ATTACH (odd
//                            sprites), bit 2 VSTART V8, bit 1 VSTOP V8,
//                            bit 0 HSTART H0. Writing CTL disarms the sprite.
//   SPRxDATA/B   $144/$146   16 pixels, 2 bits each (DATA = bit 0). Writing
//                            DATA arms the sprite.
// DMA (DMACON SPREN): after vertical blank, POS and CTL are fetched from
// SPRxPT; on lines VSTART..VSTOP-1 one DATA/DATB pair per line; at VSTOP the
// next POS/CTL pair (a 0/0 pair ends the sprite for this frame).
//
// Display: 16 lowres pixels from HSTART; colour 0 is transparent, 1-3 map to
// COLOR17-19 (sprites 0/1), 21-23 (2/3), 25-27 (4/5), 29-31 (6/7). An
// attached pair combines into 4 bits: COLOR16-31. Lower-numbered sprites are
// in front. Collisions are detected by Denise (CLXCON/CLXDAT).
//
// Mid-line changes: every register write (CPU, Copper or DMA) is logged with
// the color clock it happened at, and the line is displayed by replaying
// them. As on hardware, DATA/DATB are copied into the shift registers when
// the beam reaches HSTART (a "trigger"), so a sprite moved further right after
// being shown triggers again on the same line (horizontal multiplexing), and
// data written after a trigger is used from the next one. A write at color
// clock t applies from lowres position 2t (the beam's position then).
class Sprites {
public:
    static constexpr unsigned kCount = 8;
    static constexpr uint16_t kFirstRegister = 0x120;
    static constexpr uint16_t kLastRegister = 0x17E;
    static constexpr uint16_t kFirstDmaLine = 25;  // sprite DMA starts when vertical blank ends
    static constexpr uint32_t kAddressMask = (MemoryBus::kChipRamSize - 1) & ~1u;

    using ChipRam = std::span<const uint8_t, MemoryBus::kChipRamSize>;

    struct Sprite {
        uint32_t pointer = 0;
        uint16_t pos = 0;
        uint16_t ctl = 0;
        uint16_t data = 0;
        uint16_t datb = 0;
        bool armed = false;
        enum class Dma : uint8_t { Idle, WaitStart, Active } dma = Dma::Idle;

        [[nodiscard]] uint16_t hstart() const noexcept {  // lowres pixel position, H8-H0
            return static_cast<uint16_t>((pos & 0xFFu) << 1 | (ctl & 1u));
        }
        [[nodiscard]] uint16_t vstart() const noexcept {
            return static_cast<uint16_t>((pos >> 8) | ((ctl & 4u) << 6));
        }
        [[nodiscard]] uint16_t vstop() const noexcept {
            return static_cast<uint16_t>((ctl >> 8) | ((ctl & 2u) << 7));
        }
        [[nodiscard]] bool attached() const noexcept { return (ctl & 0x80u) != 0; }
    };

    static constexpr size_t kMaxEvents = 128;   // register writes logged per line
    static constexpr size_t kMaxTriggers = 16;  // times one sprite can start per line

    // One start of a sprite's 16 pixels on the current line.
    struct Trigger {
        uint16_t hstart;  // lowres position
        uint16_t data;
        uint16_t datb;
        bool attached;    // CTL ATTACH of this sprite at that moment
    };

    // Start of a display line: the state now is what the line starts with.
    void begin_line() noexcept {
        line_start_ = sprites_;
        event_count_ = 0;
    }

    // `hpos`: color clock of the write on the current line.
    bool write_register(uint16_t offset, uint16_t value, uint16_t hpos = 0) noexcept {
        if (offset < kFirstRegister || offset > kLastRegister) return false;
        if (offset < 0x140) {  // pointers
            Sprite& s = sprites_[(offset - 0x120) / 4];
            if ((offset & 2u) == 0) {
                s.pointer = ((uint32_t{value} << 16) | (s.pointer & 0xFFFFu)) & kAddressMask;
            } else {
                s.pointer = ((s.pointer & 0xFFFF'0000u) | value) & kAddressMask;
            }
            return true;
        }
        const auto n = static_cast<uint8_t>((offset - 0x140) / 8);
        set(n, static_cast<uint8_t>((offset & 6u) / 2), value, hpos);
        return true;
    }

    // False when no sprite can show on this line (none armed, none written).
    [[nodiscard]] bool active_on_line() const noexcept {
        if (event_count_ != 0) return true;
        for (const Sprite& s : line_start_) {
            if (s.armed) return true;
        }
        return false;
    }

    // The sprite's starts on the current line, in order. Returns how many.
    size_t triggers(unsigned n, std::span<Trigger, kMaxTriggers> out) const noexcept {
        Sprite state = line_start_[n & 7u];
        size_t count = 0;
        unsigned position = 0;  // the beam has passed everything before this
        size_t e = 0;
        for (;;) {
            while (e < event_count_ && events_[e].sprite != n) ++e;
            const unsigned next_event = e < event_count_ ? 2u * events_[e].hpos : ~0u;
            const unsigned h = state.hstart();
            if (state.armed && h >= position && h < next_event) {
                if (count < out.size()) out[count++] = {state.hstart(), state.data, state.datb, state.attached()};
                position = h + 1;
                continue;
            }
            if (e >= event_count_) break;
            if (next_event > position) position = next_event;
            apply(state, events_[e].reg, events_[e].value);
            ++e;
        }
        return count;
    }

    // Sprite DMA for line `vpos`, before the line is displayed.
    void dma_line(ChipRam chip_ram, uint16_t vpos, bool dma_enabled) noexcept {
        if (!dma_enabled || vpos < kFirstDmaLine) return;
        for (Sprite& s : sprites_) {
            if (vpos == kFirstDmaLine) fetch_control(chip_ram, s);
            if (s.dma == Sprite::Dma::WaitStart && vpos == s.vstart()) s.dma = Sprite::Dma::Active;
            if (s.dma != Sprite::Dma::Active) continue;
            if (vpos == s.vstop()) {
                fetch_control(chip_ram, s);  // next use of this sprite, or the 0/0 end marker
                if (s.dma == Sprite::Dma::WaitStart && vpos == s.vstart()) s.dma = Sprite::Dma::Active;
                if (s.dma != Sprite::Dma::Active) continue;
            }
            const auto n = static_cast<uint8_t>(&s - sprites_.data());
            set(n, 3, read(chip_ram, s.pointer + 2), kDmaHpos);  // DATB
            set(n, 2, read(chip_ram, s.pointer), kDmaHpos);      // DATA: arms the sprite
            s.pointer = (s.pointer + 4) & kAddressMask;
        }
    }

    [[nodiscard]] const Sprite& sprite(unsigned n) const noexcept { return sprites_[n & 7u]; }

private:
    static uint16_t read(ChipRam chip_ram, uint32_t address) noexcept {
        address &= kAddressMask;
        return static_cast<uint16_t>(chip_ram[address] << 8 | chip_ram[address + 1]);
    }

    static constexpr uint16_t kDmaHpos = 0x15;  // sprite DMA slots at the start of the line

    // reg: 0 POS, 1 CTL (disarms), 2 DATA (arms), 3 DATB.
    static void apply(Sprite& s, uint8_t reg, uint16_t value) noexcept {
        switch (reg) {
            case 0: s.pos = value; break;
            case 1: s.ctl = value; s.armed = false; break;
            case 2: s.data = value; s.armed = true; break;
            default: s.datb = value; break;
        }
    }

    void set(uint8_t n, uint8_t reg, uint16_t value, uint16_t hpos) noexcept {
        apply(sprites_[n], reg, value);
        if (event_count_ < kMaxEvents) events_[event_count_++] = {hpos, n, reg, value};
    }

    void fetch_control(ChipRam chip_ram, Sprite& s) noexcept {
        const auto n = static_cast<uint8_t>(&s - sprites_.data());
        set(n, 0, read(chip_ram, s.pointer), kDmaHpos);
        set(n, 1, read(chip_ram, s.pointer + 2), kDmaHpos);  // disarms
        s.pointer = (s.pointer + 4) & kAddressMask;
        s.dma = s.vstop() > s.vstart() ? Sprite::Dma::WaitStart : Sprite::Dma::Idle;
    }

    struct Event {
        uint16_t hpos;
        uint8_t sprite;
        uint8_t reg;
        uint16_t value;
    };

    std::array<Sprite, kCount> sprites_{};
    std::array<Sprite, kCount> line_start_{};
    std::array<Event, kMaxEvents> events_{};
    size_t event_count_ = 0;
};

}  // namespace amiga
