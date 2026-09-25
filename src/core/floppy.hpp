#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "core/adf.hpp"

namespace amiga {

// An Amiga 3.5" double-density floppy drive (the A500 internal DF0), as seen
// through the CIAs (Amiga Hardware Reference Manual, chapter 8).
//
// Control (CIA-B PRB outputs, active low except DIR):
//   bit 7 /MTR  motor, latched when this drive's /SEL goes low
//   bit 3 /SEL0 (DF0; bits 4-6 select DF1-DF3, which are not connected)
//   bit 2 /SIDE (low = upper head, head 1), bit 1 DIR (1 = out, towards
//   track 0), bit 0 /STEP (the head moves when it goes low)
// Status (CIA-A PRA inputs, active low), only driven while selected:
//   bit 5 /RDY  motor on and up to speed; with the motor off it shifts out
//               the drive ID instead ($FFFFFFFF for a 3.5" drive: always low)
//   bit 4 /TK0  head on cylinder 0
//   bit 3 /WPRO write protected (always, since writing is not emulated)
//   bit 2 /CHNG disk change latch: set at power-on and when the disk is
//               removed, reset by a step pulse with a disk inserted
//
// With the motor on and a disk inserted, the disk turns at 300 RPM: one MFM
// word passes the head every 32 us (113.5 color clocks), and the index hole
// pulses once per revolution (CIA-B /FLAG).
class FloppyDrive {
public:
    static constexpr uint8_t kPrbStep = 1u << 0;
    static constexpr uint8_t kPrbDir = 1u << 1;
    static constexpr uint8_t kPrbSide = 1u << 2;
    static constexpr uint8_t kPrbSel0 = 1u << 3;
    static constexpr uint8_t kPrbMtr = 1u << 7;

    static constexpr uint8_t kPraChng = 1u << 2;
    static constexpr uint8_t kPraWpro = 1u << 3;
    static constexpr uint8_t kPraTk0 = 1u << 4;
    static constexpr uint8_t kPraRdy = 1u << 5;
    static constexpr uint8_t kStatusBits = kPraChng | kPraWpro | kPraTk0 | kPraRdy;

    static constexpr unsigned kCylinders = AdfImage::kCylinders;
    // Word time in half color clocks: 32 us * 3.546895 MHz * 2 = 227.
    static constexpr unsigned kHalfColorClocksPerWord = 227;

    // What passed under the head during one color clock.
    struct Tick {
        bool word_ready = false;  // a new MFM word is available (drive selected)
        uint16_t word = 0;
        bool index = false;       // index pulse (drive selected)
    };

    void insert(std::unique_ptr<AdfImage> disk) noexcept {
        disk_ = std::move(disk);
        encoded_track_ = -1;
    }
    void eject() noexcept {
        disk_.reset();
        change_latch_ = true;
    }

    // New levels on the CIA-B port B pins.
    void control(uint8_t prb) noexcept {
        const bool selected = (prb & kPrbSel0) == 0;
        if (selected && !selected_) motor_ = (prb & kPrbMtr) == 0;  // /MTR latched on selection
        if (selected && (prev_prb_ & kPrbStep) != 0 && (prb & kPrbStep) == 0) step((prb & kPrbDir) != 0);
        head_ = (prb & kPrbSide) == 0 ? 1u : 0u;
        selected_ = selected;
        prev_prb_ = prb;
    }

    // Levels this drive puts on CIA-A PRA bits 2-5 (1 = high/inactive).
    [[nodiscard]] uint8_t status() const noexcept {
        if (!selected_) return kStatusBits;  // not driving the lines: pulled up
        uint8_t lines = kStatusBits;
        lines &= static_cast<uint8_t>(~kPraRdy);  // ID all ones / motor ready
        lines &= static_cast<uint8_t>(~kPraWpro);  // writing not emulated: always protected
        if (cylinder_ == 0) lines &= static_cast<uint8_t>(~kPraTk0);
        if (change_latch_) lines &= static_cast<uint8_t>(~kPraChng);
        return lines;
    }

    // One color clock of disk rotation.
    Tick tick() noexcept {
        Tick result;
        if (!motor_ || !disk_) return result;
        rotation_ += 2;
        if (rotation_ < kHalfColorClocksPerWord) return result;
        rotation_ -= kHalfColorClocksPerWord;
        if (++position_ == AdfImage::kTrackWords) {
            position_ = 0;
            result.index = selected_;
        }
        if (!selected_) return result;  // only the selected drive drives /RDATA
        const int track = static_cast<int>(cylinder_ * 2 + head_);
        if (track != encoded_track_) {
            disk_->encode_track(static_cast<unsigned>(track), track_);
            encoded_track_ = track;
        }
        result.word_ready = true;
        result.word = track_[position_];
        return result;
    }

    [[nodiscard]] bool motor_on() const noexcept { return motor_; }
    [[nodiscard]] bool selected() const noexcept { return selected_; }
    [[nodiscard]] unsigned cylinder() const noexcept { return cylinder_; }
    [[nodiscard]] unsigned head() const noexcept { return head_; }
    [[nodiscard]] bool disk_inserted() const noexcept { return disk_ != nullptr; }

private:
    void step(bool outwards) noexcept {
        if (outwards) {
            if (cylinder_ > 0) --cylinder_;
        } else if (cylinder_ + 1 < kCylinders) {
            ++cylinder_;
        }
        if (disk_) change_latch_ = false;
    }

    uint8_t prev_prb_ = 0xFF;
    bool selected_ = false;
    bool motor_ = false;
    unsigned cylinder_ = 0;
    unsigned head_ = 0;
    bool change_latch_ = true;  // power-on

    std::unique_ptr<AdfImage> disk_;
    std::array<uint16_t, AdfImage::kTrackWords> track_{};
    int encoded_track_ = -1;
    unsigned position_ = 0;
    unsigned rotation_ = 0;  // half color clocks towards the next word
};

}  // namespace amiga
