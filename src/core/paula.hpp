#pragma once

#include <cstdint>
#include <initializer_list>

#include "core/custom_registers.hpp"

namespace amiga {

// Paula (8364) — interrupt controller and the pot pins (POTGO/POTGOR) so far.
//
// INTREQ holds pending requests, INTENA enables them; INTEN (bit 14) is the
// master switch. Paula encodes the highest enabled, pending request onto the
// 68000 IPL lines (levels 1-6; the Amiga uses autovectors).
class Paula {
public:
    static constexpr uint16_t kIntBits = 0x7FFF;

    // POTGO/POTGOR pin bits: OUTxx enables driving the pin with DATxx.
    static constexpr uint16_t kDatLx = 1u << 8;   // port 0 pin 5: middle mouse button
    static constexpr uint16_t kDatLy = 1u << 10;  // port 0 pin 9: right mouse button
    static constexpr uint16_t kDatRx = 1u << 12;
    static constexpr uint16_t kDatRy = 1u << 14;

    bool write_register(uint16_t offset, uint16_t value) noexcept {
        switch (offset) {
            case reg::kIntEna: apply_set_clr(intena_, value); return true;
            case reg::kIntReq: apply_set_clr(intreq_, value); return true;
            case reg::kPotGo: potgo_ = value; return true;  // START (pot counters) not emulated
            default: return false;
        }
    }

    // Returns false if the register isn't a readable Paula register.
    bool read_register(uint16_t offset, uint16_t& value) const noexcept {
        switch (offset) {
            case reg::kIntEnaR: value = intena_; return true;
            case reg::kIntReqR: value = intreq_; return true;
            case reg::kPotGoR: value = pot_pins(); return true;
            default: return false;
        }
    }

    // Mouse buttons wired to the port 0 pot pins (pressed = pulled low).
    void set_mouse_buttons(bool right, bool middle) noexcept {
        pulled_low_ = static_cast<uint16_t>((right ? kDatLy : 0) | (middle ? kDatLx : 0));
    }

    // Pin levels: driven by POTGO when its OUTxx bit (DATxx << 1) is set,
    // otherwise pulled up; a pressed button pulls the pin low either way.
    [[nodiscard]] uint16_t pot_pins() const noexcept {
        uint16_t pins = 0;
        for (const uint16_t dat : {kDatLx, kDatLy, kDatRx, kDatRy}) {
            const bool output = (potgo_ & (dat << 1)) != 0;
            const bool high = output ? (potgo_ & dat) != 0 : true;
            if (high && (pulled_low_ & dat) == 0) pins |= dat;
        }
        return pins;
    }

    // Raised by hardware sources (vertical blank, blitter, CIAs...).
    void request(uint16_t bits) noexcept { intreq_ |= bits & kIntBits; }

    [[nodiscard]] uint16_t intena() const noexcept { return intena_; }
    [[nodiscard]] uint16_t intreq() const noexcept { return intreq_; }

    // 68000 interrupt priority level currently requested (0 = none).
    [[nodiscard]] uint8_t interrupt_level() const noexcept {
        if ((intena_ & reg::kIntEn) == 0) return 0;
        const uint16_t active = intena_ & intreq_;
        if ((active & reg::kIntExter) != 0) return 6;
        if ((active & (reg::kIntDskSyn | reg::kIntRbf)) != 0) return 5;
        if ((active & 0x0780u) != 0) return 4;  // AUD0-AUD3
        if ((active & (reg::kIntBlit | reg::kIntVertb | reg::kIntCoper)) != 0) return 3;
        if ((active & reg::kIntPorts) != 0) return 2;
        if ((active & (reg::kIntSoft | reg::kIntDskBlk | reg::kIntTbe)) != 0) return 1;
        return 0;
    }

private:
    static void apply_set_clr(uint16_t& reg_value, uint16_t value) noexcept {
        const uint16_t bits = value & kIntBits;
        if ((value & reg::kSetClr) != 0) {
            reg_value |= bits;
        } else {
            reg_value &= static_cast<uint16_t>(~bits);
        }
    }

    uint16_t intena_ = 0;
    uint16_t intreq_ = 0;
    uint16_t potgo_ = 0;
    uint16_t pulled_low_ = 0;
};

}  // namespace amiga
