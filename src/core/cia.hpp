#pragma once

#include <array>
#include <cstdint>

#include "core/floppy.hpp"
#include "core/memory_bus.hpp"

namespace amiga {

// MOS 8520 Complex Interface Adapter (Amiga Hardware Reference Manual,
// appendix F). Two 8-bit ports with data direction registers, two 16-bit
// interval timers, a 24-bit binary time-of-day counter with alarm, a serial
// shift register and the interrupt control register.
//
// Timers count E clock ticks (tick()), the TOD counts pulses on its TOD pin
// (tod_pulse()). Not emulated: the PB6/PB7 timer outputs (PBON), counting
// CNT pulses, and shifting data out of the serial port.
class Cia8520 {
public:
    enum Register : uint8_t {
        kPra = 0x0, kPrb = 0x1, kDdra = 0x2, kDdrb = 0x3,
        kTaLo = 0x4, kTaHi = 0x5, kTbLo = 0x6, kTbHi = 0x7,
        kTodLo = 0x8, kTodMid = 0x9, kTodHi = 0xA,
        kSdr = 0xC, kIcr = 0xD, kCra = 0xE, kCrb = 0xF,
    };

    // ICR bits.
    static constexpr uint8_t kIcrTa = 1u << 0;
    static constexpr uint8_t kIcrTb = 1u << 1;
    static constexpr uint8_t kIcrAlarm = 1u << 2;
    static constexpr uint8_t kIcrSp = 1u << 3;
    static constexpr uint8_t kIcrFlag = 1u << 4;
    static constexpr uint8_t kIcrIr = 1u << 7;  // read: interrupt requested; write: SET/CLR

    // CRA/CRB bits.
    static constexpr uint8_t kCrStart = 1u << 0;
    static constexpr uint8_t kCrRunMode = 1u << 3;  // 1 = one-shot
    static constexpr uint8_t kCrLoad = 1u << 4;     // strobe: force latch into counter
    static constexpr uint8_t kCraInMode = 1u << 5;  // 1 = count CNT pulses
    static constexpr uint8_t kCraSpMode = 1u << 6;  // 1 = serial port output
    static constexpr uint8_t kCrbInModeShift = 5;   // 00 E clock, 01 CNT, 10/11 timer A underflow
    static constexpr uint8_t kCrbAlarm = 1u << 7;   // 1 = TOD writes set the alarm

    Cia8520() noexcept { reset(); }

    // /RES: ports become inputs, timers stop with latches at $FFFF.
    void reset() noexcept;

    [[nodiscard]] uint8_t read(unsigned reg) noexcept;
    // Debugger read: no side effects (ICR not cleared, TOD not latched).
    [[nodiscard]] uint8_t peek(unsigned reg) const noexcept;
    void write(unsigned reg, uint8_t value) noexcept;

    void tick() noexcept;       // one E clock (709379 Hz on PAL)
    void tod_pulse() noexcept;  // one pulse on the TOD pin
    void flag_pulse() noexcept { icr_data_ |= kIcrFlag; }  // falling edge on /FLAG

    // A byte shifted in through SP/CNT (the keyboard on CIA-A). Ignored while
    // the serial port is in output mode.
    void serial_input(uint8_t byte) noexcept;

    // Levels on the port pins driven from outside (1 = high).
    void set_port_a_input(uint8_t pins) noexcept { input_a_ = pins; }
    void set_port_b_input(uint8_t pins) noexcept { input_b_ = pins; }

    // Levels the CIA drives on its port pins; inputs read as high (pull-ups).
    [[nodiscard]] uint8_t port_a_output() const noexcept {
        return static_cast<uint8_t>((pra_ & ddra_) | ~ddra_);
    }
    [[nodiscard]] uint8_t port_b_output() const noexcept {
        return static_cast<uint8_t>((prb_ & ddrb_) | ~ddrb_);
    }

    // /IRQ asserted: an enabled interrupt source is pending.
    [[nodiscard]] bool irq() const noexcept { return (icr_data_ & icr_mask_) != 0; }
    [[nodiscard]] bool serial_output_mode() const noexcept { return (cra_ & kCraSpMode) != 0; }

    [[nodiscard]] uint16_t timer_a() const noexcept { return timer_a_.counter; }
    [[nodiscard]] uint16_t timer_b() const noexcept { return timer_b_.counter; }
    [[nodiscard]] uint32_t tod() const noexcept { return tod_; }
    [[nodiscard]] uint8_t icr_mask() const noexcept { return icr_mask_; }

private:
    struct Timer {
        uint16_t counter = 0xFFFF;
        uint16_t latch = 0xFFFF;
    };

    // Counts one tick; returns true on underflow (counter reloaded from the latch).
    static bool count(Timer& timer, uint8_t& control) noexcept;
    void write_timer_high(Timer& timer, uint8_t& control, uint8_t value) noexcept;
    void write_control(Timer& timer, uint8_t& control, uint8_t value) noexcept;
    void write_tod(unsigned shift, uint8_t value) noexcept;

    uint8_t pra_ = 0, prb_ = 0, ddra_ = 0, ddrb_ = 0;
    uint8_t input_a_ = 0xFF, input_b_ = 0xFF;
    Timer timer_a_, timer_b_;
    uint8_t cra_ = 0, crb_ = 0;
    uint8_t icr_data_ = 0, icr_mask_ = 0;
    uint8_t sdr_ = 0;
    uint32_t tod_ = 0, tod_latch_ = 0, alarm_ = 0;
    bool tod_latched_ = false;  // TOD HI was read: reads come from tod_latch_ until TOD LO
    bool tod_halted_ = false;   // TOD HI was written: counting stops until TOD LO
};

// The two CIAs as the A500 wires them, decoded in the $BF0000 bank:
//   CIA-A: selected by A12 = 0, odd addresses (D0-D7),  $BFE001 + reg * $100
//   CIA-B: selected by A13 = 0, even addresses (D8-D15), $BFD000 + reg * $100
// CIA-A /IRQ drives INT2 (Paula PORTS, level 2), CIA-B /IRQ drives INT6
// (EXTER, level 6). CIA-A PA0 is the ROM overlay (OVL) line, PA6 the port 0
// fire button (left mouse button), PA2-PA5 the floppy status lines; CIA-B
// port B controls the floppy drives, and the disk index pulse arrives on
// CIA-B /FLAG. Only DF0 is connected.
// The $A00000-$BEFFFF mirrors of the CIAs are not decoded.
class Cias final : public CiaPort {
public:
    static constexpr uint8_t kPraOvl = 1u << 0;
    static constexpr uint8_t kPraFir0 = 1u << 6;  // port 0 fire / left mouse button, active low
    static constexpr uint8_t kPraFir1 = 1u << 7;  // port 1 fire, active low

    explicit Cias(MemoryBus& bus) noexcept : bus_(bus) {}

    bool read_cia(uint32_t address, uint8_t& value) override;
    bool write_cia(uint32_t address, uint8_t value) override;

    // Resets both chips; the overlay comes back on (PA0 is an input, pulled up).
    void reset() noexcept;

    // Left mouse button on port 0 (CIA-A PA6).
    void set_left_button(bool pressed) noexcept;
    // Joystick fire button on port 1 (CIA-A PA7).
    void set_joystick_fire(bool pressed) noexcept {
        joystick_fire_ = pressed;
        update_port_a_inputs();
    }

    [[nodiscard]] FloppyDrive& df0() noexcept { return df0_; }
    [[nodiscard]] Cia8520& a() noexcept { return a_; }
    [[nodiscard]] Cia8520& b() noexcept { return b_; }

private:
    void update_overlay() noexcept { bus_.set_overlay((a_.port_a_output() & kPraOvl) != 0); }
    // Levels on CIA-A port A inputs: fire buttons and floppy status.
    void update_port_a_inputs() noexcept {
        uint8_t pins = static_cast<uint8_t>(0xFF & ~FloppyDrive::kStatusBits) | df0_.status();
        if (left_button_) pins &= static_cast<uint8_t>(~kPraFir0);
        if (joystick_fire_) pins &= static_cast<uint8_t>(~kPraFir1);
        a_.set_port_a_input(pins);
    }

    MemoryBus& bus_;
    Cia8520 a_;
    Cia8520 b_;
    FloppyDrive df0_;
    bool left_button_ = false;
    bool joystick_fire_ = false;
};

}  // namespace amiga
