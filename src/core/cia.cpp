#include "core/cia.hpp"

namespace amiga {

// --- Cia8520 ------------------------------------------------------------------

void Cia8520::reset() noexcept {
    pra_ = prb_ = ddra_ = ddrb_ = 0;
    timer_a_ = Timer{};
    timer_b_ = Timer{};
    cra_ = crb_ = 0;
    icr_data_ = icr_mask_ = 0;
    sdr_ = 0;
    tod_ = tod_latch_ = alarm_ = 0;
    tod_latched_ = tod_halted_ = false;
}

uint8_t Cia8520::read(unsigned reg) noexcept {
    switch (reg & 0xFu) {
        case kPra: return static_cast<uint8_t>((pra_ & ddra_) | (input_a_ & ~ddra_));
        case kPrb: return static_cast<uint8_t>((prb_ & ddrb_) | (input_b_ & ~ddrb_));
        case kDdra: return ddra_;
        case kDdrb: return ddrb_;
        case kTaLo: return static_cast<uint8_t>(timer_a_.counter);
        case kTaHi: return static_cast<uint8_t>(timer_a_.counter >> 8);
        case kTbLo: return static_cast<uint8_t>(timer_b_.counter);
        case kTbHi: return static_cast<uint8_t>(timer_b_.counter >> 8);
        case kTodHi:  // latches the whole counter until TOD LO is read
            tod_latch_ = tod_;
            tod_latched_ = true;
            return static_cast<uint8_t>(tod_latch_ >> 16);
        case kTodMid: return static_cast<uint8_t>((tod_latched_ ? tod_latch_ : tod_) >> 8);
        case kTodLo: {
            const auto value = static_cast<uint8_t>(tod_latched_ ? tod_latch_ : tod_);
            tod_latched_ = false;
            return value;
        }
        case kSdr: return sdr_;
        case kIcr: {  // reading clears all flags (and releases /IRQ)
            const auto value = static_cast<uint8_t>(icr_data_ | (irq() ? kIcrIr : 0));
            icr_data_ = 0;
            return value;
        }
        case kCra: return cra_;
        case kCrb: return crb_;
        default: return 0xFF;  // register $B is not connected
    }
}

uint8_t Cia8520::peek(unsigned reg) const noexcept {
    switch (reg & 0xFu) {
        case kIcr: return static_cast<uint8_t>(icr_data_ | (irq() ? kIcrIr : 0));
        case kTodHi: return static_cast<uint8_t>(tod_ >> 16);
        case kTodMid: return static_cast<uint8_t>(tod_ >> 8);
        case kTodLo: return static_cast<uint8_t>(tod_);
        default: return const_cast<Cia8520*>(this)->read(reg);  // no side effects for the others
    }
}

void Cia8520::write(unsigned reg, uint8_t value) noexcept {
    switch (reg & 0xFu) {
        case kPra: pra_ = value; break;
        case kPrb: prb_ = value; break;
        case kDdra: ddra_ = value; break;
        case kDdrb: ddrb_ = value; break;
        case kTaLo: timer_a_.latch = static_cast<uint16_t>((timer_a_.latch & 0xFF00u) | value); break;
        case kTaHi: write_timer_high(timer_a_, cra_, value); break;
        case kTbLo: timer_b_.latch = static_cast<uint16_t>((timer_b_.latch & 0xFF00u) | value); break;
        case kTbHi: write_timer_high(timer_b_, crb_, value); break;
        case kTodLo: write_tod(0, value); break;
        case kTodMid: write_tod(8, value); break;
        case kTodHi: write_tod(16, value); break;
        case kSdr: sdr_ = value; break;
        case kIcr: {
            const uint8_t bits = value & 0x1Fu;
            icr_mask_ = (value & kIcrIr) != 0 ? static_cast<uint8_t>(icr_mask_ | bits)
                                              : static_cast<uint8_t>(icr_mask_ & ~bits);
            break;
        }
        case kCra: write_control(timer_a_, cra_, value); break;
        case kCrb: write_control(timer_b_, crb_, value); break;
        default: break;
    }
}

// Writing the high byte: a stopped timer is loaded from the latch. In one-shot
// mode the 8520 also loads and starts the timer, whatever the START bit.
void Cia8520::write_timer_high(Timer& timer, uint8_t& control, uint8_t value) noexcept {
    timer.latch = static_cast<uint16_t>((timer.latch & 0x00FFu) | uint32_t{value} << 8);
    if ((control & kCrRunMode) != 0) {
        timer.counter = timer.latch;
        control |= kCrStart;
    } else if ((control & kCrStart) == 0) {
        timer.counter = timer.latch;
    }
}

void Cia8520::write_control(Timer& timer, uint8_t& control, uint8_t value) noexcept {
    if ((value & kCrLoad) != 0) timer.counter = timer.latch;  // strobe, not stored
    control = static_cast<uint8_t>(value & ~kCrLoad);
}

// With CRB ALARM set, writes go to the alarm; otherwise writing TOD HI halts
// the counter until TOD LO is written.
void Cia8520::write_tod(unsigned shift, uint8_t value) noexcept {
    uint32_t& target = (crb_ & kCrbAlarm) != 0 ? alarm_ : tod_;
    target = (target & ~(0xFFu << shift)) | uint32_t{value} << shift;
    if (&target == &tod_) {
        if (shift == 16) tod_halted_ = true;
        if (shift == 0) tod_halted_ = false;
    }
}

bool Cia8520::count(Timer& timer, uint8_t& control) noexcept {
    if ((control & kCrStart) == 0) return false;
    if (timer.counter != 0) {
        --timer.counter;
        return false;
    }
    timer.counter = timer.latch;  // underflow: N + 1 ticks per period
    if ((control & kCrRunMode) != 0) control &= static_cast<uint8_t>(~kCrStart);
    return true;
}

void Cia8520::tick() noexcept {
    bool a_underflow = false;
    if ((cra_ & kCraInMode) == 0) {  // CNT counting not emulated
        a_underflow = count(timer_a_, cra_);
        if (a_underflow) icr_data_ |= kIcrTa;
    }
    const unsigned b_mode = (crb_ >> kCrbInModeShift) & 3u;
    // 00: E clock. 10: timer A underflows. 11: timer A underflows while CNT
    // is high; CNT idles high on the Amiga, so it behaves like 10.
    const bool b_input = b_mode == 0 || (b_mode >= 2 && a_underflow);
    if (b_input && count(timer_b_, crb_)) icr_data_ |= kIcrTb;
}

void Cia8520::tod_pulse() noexcept {
    if (tod_halted_) return;
    tod_ = (tod_ + 1) & 0xFF'FFFFu;
    if (tod_ == alarm_) icr_data_ |= kIcrAlarm;
}

void Cia8520::serial_input(uint8_t byte) noexcept {
    if ((cra_ & kCraSpMode) != 0) return;
    sdr_ = byte;
    icr_data_ |= kIcrSp;
}

// --- Cias -----------------------------------------------------------------------

bool Cias::read_cia(uint32_t address, uint8_t& value) {
    const unsigned reg = (address >> 8) & 0xFu;
    if ((address & 1u) != 0) {
        if ((address & 0x1000u) != 0) return false;  // CIA-A needs A12 = 0
        value = a_.read(reg);
    } else {
        if ((address & 0x2000u) != 0) return false;  // CIA-B needs A13 = 0
        value = b_.read(reg);
    }
    return true;
}

bool Cias::write_cia(uint32_t address, uint8_t value) {
    const unsigned reg = (address >> 8) & 0xFu;
    if ((address & 1u) != 0) {
        if ((address & 0x1000u) != 0) return false;
        a_.write(reg, value);
        if (reg == Cia8520::kPra || reg == Cia8520::kDdra) update_overlay();
    } else {
        if ((address & 0x2000u) != 0) return false;
        b_.write(reg, value);
        if (reg == Cia8520::kPrb || reg == Cia8520::kDdrb) {
            df0_.control(b_.port_b_output());
            update_port_a_inputs();
        }
    }
    return true;
}

void Cias::reset() noexcept {
    a_.reset();
    b_.reset();
    update_overlay();
    df0_.control(b_.port_b_output());  // port B floats high: drives deselected
    update_port_a_inputs();
}

void Cias::set_left_button(bool pressed) noexcept {
    left_button_ = pressed;
    update_port_a_inputs();
}

}  // namespace amiga
