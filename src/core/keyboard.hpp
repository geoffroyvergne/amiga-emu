#pragma once

#include <array>
#include <cstdint>

#include "core/cia.hpp"

namespace amiga {

// The A500 keyboard as seen through CIA-A's serial port (HRM chapter 8).
//
// Each key transition is sent as one byte: raw keycode in bits 6-0, bit 7 set
// on release. On the wire it is rotated left by one and inverted, so the byte
// software reads from SDR is ~((code << 1) | up); it then shows up as the
// CIA-A SP interrupt (level 2).
//
// After each byte the keyboard waits for the handshake: the CPU switches the
// serial port to output (CRA SPMODE), which pulls KDAT low. If no handshake
// comes within 143 ms the keyboard sends the next byte anyway (the real
// keyboard's resync is not modelled). Bits are delivered as one byte, not
// clocked in one by one.
class Keyboard {
public:
    static constexpr uint8_t kCapsLock = 0x62;
    static constexpr uint8_t kInitiatePowerUp = 0xFD;  // start of power-up key stream
    static constexpr uint8_t kTerminatePowerUp = 0xFE;  // end of power-up key stream
    static constexpr uint32_t kHandshakeTimeout = 101'441;  // 143 ms in E clocks

    // Encoding of a raw code (code | 0x80 for release) as read from SDR.
    [[nodiscard]] static constexpr uint8_t encode(uint8_t raw) noexcept {
        return static_cast<uint8_t>(~((raw << 1) | (raw >> 7)));
    }

    // Keyboard reset: pending keys dropped, power-up stream queued.
    void reset() noexcept {
        head_ = tail_ = 0;
        awaiting_handshake_ = false;
        caps_lock_ = false;
        push(kInitiatePowerUp);
        push(kTerminatePowerUp);
    }

    // A key changed state. Caps Lock only reports its toggled state: down when
    // it turns on, up when it turns off; releasing the key sends nothing.
    void key(uint8_t code, bool down) noexcept {
        code &= 0x7Fu;
        if (code == kCapsLock) {
            if (!down) return;
            caps_lock_ = !caps_lock_;
            push(static_cast<uint8_t>(code | (caps_lock_ ? 0 : 0x80)));
            return;
        }
        push(static_cast<uint8_t>(code | (down ? 0 : 0x80)));
    }

    // One E clock.
    void tick(Cia8520& cia) noexcept {
        if (awaiting_handshake_) {
            if (cia.serial_output_mode() || --timeout_ == 0) awaiting_handshake_ = false;
            return;
        }
        if (head_ == tail_ || cia.serial_output_mode()) return;  // nothing to send / line busy
        cia.serial_input(encode(queue_[tail_]));
        tail_ = (tail_ + 1) % queue_.size();
        awaiting_handshake_ = true;
        timeout_ = kHandshakeTimeout;
    }

    [[nodiscard]] bool caps_lock() const noexcept { return caps_lock_; }
    [[nodiscard]] bool awaiting_handshake() const noexcept { return awaiting_handshake_; }
    [[nodiscard]] size_t pending() const noexcept {
        return (head_ + queue_.size() - tail_) % queue_.size();
    }

private:
    // Like the keyboard's own buffer, a full queue drops new events.
    void push(uint8_t raw) noexcept {
        const size_t next = (head_ + 1) % queue_.size();
        if (next == tail_) return;
        queue_[head_] = raw;
        head_ = next;
    }

    std::array<uint8_t, 32> queue_{};
    size_t head_ = 0;
    size_t tail_ = 0;
    bool awaiting_handshake_ = false;
    uint32_t timeout_ = 0;
    bool caps_lock_ = false;
};

}  // namespace amiga
