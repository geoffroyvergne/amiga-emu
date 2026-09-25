#include <cstdint>
#include <memory>
#include <vector>

#include "core/cia.hpp"
#include "core/custom_registers.hpp"
#include "core/keyboard.hpp"
#include "core/machine.hpp"
#include "core/memory_bus.hpp"
#include "core/paula.hpp"
#include "test_framework.hpp"

namespace {

using amiga::BusError;
using amiga::Cias;
using amiga::Cia8520;
using amiga::Keyboard;
using amiga::Machine;
using amiga::MemoryBus;
using amiga::Paula;
namespace reg = amiga::reg;

constexpr uint32_t kCiaA = 0xBFE001;  // + reg * $100
constexpr uint32_t kCiaB = 0xBFD000;

void tick(Cia8520& cia, unsigned n) {
    for (unsigned i = 0; i < n; ++i) cia.tick();
}

// --- 8520 ------------------------------------------------------------------------

void test_ports_and_data_direction() {
    Cia8520 cia;
    cia.set_port_a_input(0xBF);  // external pin 6 low
    EXPECT(cia.read(Cia8520::kPra) == 0xBF);  // all inputs after reset
    cia.write(Cia8520::kDdra, 0x03);
    cia.write(Cia8520::kPra, 0x02);
    EXPECT(cia.read(Cia8520::kPra) == 0xBE);  // bits 1-0 from the latch, others from pins
    EXPECT(cia.port_a_output() == 0xFE);      // inputs float high
}

void test_timer_continuous() {
    Cia8520 cia;
    cia.write(Cia8520::kTaLo, 4);
    cia.write(Cia8520::kTaHi, 0);  // stopped: latch -> counter
    EXPECT(cia.timer_a() == 4);
    cia.write(Cia8520::kCra, Cia8520::kCrStart);

    tick(cia, 4);
    EXPECT(cia.timer_a() == 0);
    EXPECT(cia.read(Cia8520::kIcr) == 0);
    cia.tick();  // underflow: period is latch + 1 ticks
    EXPECT(cia.timer_a() == 4);
    EXPECT(cia.read(Cia8520::kIcr) == Cia8520::kIcrTa);  // no mask: IR not set
    EXPECT(cia.read(Cia8520::kIcr) == 0);                // reading cleared it
    tick(cia, 5);
    EXPECT(cia.timer_a() == 4);  // keeps running

    cia.write(Cia8520::kTaLo, 9);
    cia.write(Cia8520::kTaHi, 0);  // running: counter not reloaded
    EXPECT(cia.timer_a() == 4);
}

void test_timer_interrupt_mask() {
    Cia8520 cia;
    cia.write(Cia8520::kIcr, Cia8520::kIcrIr | Cia8520::kIcrTa);  // SET TA
    EXPECT(cia.icr_mask() == Cia8520::kIcrTa);
    cia.write(Cia8520::kTaLo, 1);
    cia.write(Cia8520::kTaHi, 0);
    cia.write(Cia8520::kCra, Cia8520::kCrStart);
    tick(cia, 2);
    EXPECT(cia.irq());
    EXPECT(cia.read(Cia8520::kIcr) == (Cia8520::kIcrIr | Cia8520::kIcrTa));
    EXPECT(!cia.irq());

    cia.write(Cia8520::kIcr, Cia8520::kIcrTa);  // CLR TA
    EXPECT(cia.icr_mask() == 0);
}

void test_timer_one_shot() {
    Cia8520 cia;
    cia.write(Cia8520::kCrb, Cia8520::kCrRunMode);  // one-shot, not started
    cia.write(Cia8520::kTbLo, 2);
    cia.write(Cia8520::kTbHi, 0);  // 8520: loads and starts in one-shot mode
    EXPECT((cia.read(Cia8520::kCrb) & Cia8520::kCrStart) != 0);
    tick(cia, 3);
    EXPECT(cia.read(Cia8520::kIcr) == Cia8520::kIcrTb);
    EXPECT((cia.read(Cia8520::kCrb) & Cia8520::kCrStart) == 0);  // stopped
    EXPECT(cia.timer_b() == 2);  // reloaded
    tick(cia, 10);
    EXPECT(cia.read(Cia8520::kIcr) == 0);
}

void test_timer_force_load() {
    Cia8520 cia;
    cia.write(Cia8520::kTaLo, 0x34);
    cia.write(Cia8520::kTaHi, 0x12);
    cia.write(Cia8520::kCra, Cia8520::kCrStart);
    tick(cia, 0x100);
    cia.write(Cia8520::kCra, Cia8520::kCrStart | Cia8520::kCrLoad);
    EXPECT(cia.timer_a() == 0x1234);
    EXPECT(cia.read(Cia8520::kCra) == Cia8520::kCrStart);  // LOAD is a strobe
    EXPECT(cia.read(Cia8520::kTaHi) == 0x12);
    EXPECT(cia.read(Cia8520::kTaLo) == 0x34);
}

void test_timer_b_counts_timer_a_underflows() {
    Cia8520 cia;
    cia.write(Cia8520::kTaLo, 0);  // underflows every tick
    cia.write(Cia8520::kTaHi, 0);
    cia.write(Cia8520::kTbLo, 1);
    cia.write(Cia8520::kTbHi, 0);
    cia.write(Cia8520::kCrb, static_cast<uint8_t>(Cia8520::kCrStart | 2u << Cia8520::kCrbInModeShift));
    tick(cia, 5);
    EXPECT(cia.timer_b() == 1);  // timer A not started: no input
    cia.write(Cia8520::kCra, Cia8520::kCrStart);
    tick(cia, 2);
    EXPECT((cia.read(Cia8520::kIcr) & Cia8520::kIcrTb) != 0);
}

void test_time_of_day() {
    Cia8520 cia;
    for (int i = 0; i < 0x010203; ++i) cia.tod_pulse();
    EXPECT(cia.read(Cia8520::kTodHi) == 0x01);  // latches
    cia.tod_pulse();
    EXPECT(cia.read(Cia8520::kTodMid) == 0x02);
    EXPECT(cia.read(Cia8520::kTodLo) == 0x03);  // releases the latch
    EXPECT(cia.read(Cia8520::kTodLo) == 0x04);

    cia.write(Cia8520::kTodHi, 0x00);  // halts counting...
    cia.write(Cia8520::kTodMid, 0x00);
    cia.tod_pulse();
    EXPECT(cia.tod() == 0x000004);
    cia.write(Cia8520::kTodLo, 0x00);  // ...until LO is written
    cia.tod_pulse();
    EXPECT(cia.tod() == 1);

    cia.write(Cia8520::kCrb, Cia8520::kCrbAlarm);  // writes go to the alarm
    cia.write(Cia8520::kTodHi, 0);
    cia.write(Cia8520::kTodMid, 0);
    cia.write(Cia8520::kTodLo, 5);
    cia.write(Cia8520::kCrb, 0);
    EXPECT(cia.tod() == 1);
    for (int i = 0; i < 4; ++i) cia.tod_pulse();
    EXPECT(cia.read(Cia8520::kIcr) == Cia8520::kIcrAlarm);

    Cia8520 wrap;
    wrap.write(Cia8520::kTodHi, 0xFF);
    wrap.write(Cia8520::kTodMid, 0xFF);
    wrap.write(Cia8520::kTodLo, 0xFF);
    wrap.tod_pulse();
    EXPECT(wrap.tod() == 0);  // 24-bit binary counter
}

void test_serial_input() {
    Cia8520 cia;
    cia.serial_input(0x55);
    EXPECT(cia.read(Cia8520::kSdr) == 0x55);
    EXPECT(cia.read(Cia8520::kIcr) == Cia8520::kIcrSp);

    cia.write(Cia8520::kCra, Cia8520::kCraSpMode);  // output mode: input ignored
    cia.serial_input(0xAA);
    EXPECT(cia.read(Cia8520::kSdr) == 0x55);
}

// --- Keyboard --------------------------------------------------------------------

uint8_t decode(uint8_t sdr) {  // what keyboard.device does: NOT, then ROR #1
    const auto inverted = static_cast<uint8_t>(~sdr);
    return static_cast<uint8_t>(inverted >> 1 | inverted << 7);
}

void test_keyboard_encoding() {
    EXPECT(Keyboard::encode(0x45) == 0x75);  // Esc down
    EXPECT(Keyboard::encode(0xC5) == 0x74);  // Esc up
    for (unsigned raw = 0; raw < 256; ++raw) {
        EXPECT(decode(Keyboard::encode(static_cast<uint8_t>(raw))) == raw);
    }
}

void test_keyboard_handshake() {
    Cia8520 cia;
    Keyboard kb;
    kb.reset();  // power-up stream: $FD, $FE
    kb.tick(cia);
    EXPECT(decode(cia.read(Cia8520::kSdr)) == Keyboard::kInitiatePowerUp);
    EXPECT(cia.read(Cia8520::kIcr) == Cia8520::kIcrSp);

    kb.tick(cia);  // waiting for the handshake: nothing new
    EXPECT(cia.read(Cia8520::kIcr) == 0);

    cia.write(Cia8520::kCra, Cia8520::kCraSpMode);  // handshake: KDAT pulled low
    kb.tick(cia);
    kb.tick(cia);  // line still held by the CPU
    EXPECT(cia.read(Cia8520::kIcr) == 0);
    cia.write(Cia8520::kCra, 0);  // released
    kb.tick(cia);
    EXPECT(decode(cia.read(Cia8520::kSdr)) == Keyboard::kTerminatePowerUp);

    kb.key(0x20, true);  // 'A': no handshake this time, the keyboard times out
    for (uint32_t i = 0; i < Keyboard::kHandshakeTimeout; ++i) kb.tick(cia);
    EXPECT(kb.pending() == 1);
    kb.tick(cia);
    EXPECT(decode(cia.read(Cia8520::kSdr)) == 0x20);
    EXPECT(kb.pending() == 0);
}

void test_keyboard_caps_lock_toggles() {
    Cia8520 cia;
    Keyboard kb;
    kb.key(Keyboard::kCapsLock, true);   // on: sends "down"
    kb.key(Keyboard::kCapsLock, false);  // release: sends nothing
    kb.key(Keyboard::kCapsLock, true);   // off: sends "up"
    EXPECT(kb.pending() == 2);
    kb.tick(cia);
    EXPECT(decode(cia.read(Cia8520::kSdr)) == 0x62);
    cia.write(Cia8520::kCra, Cia8520::kCraSpMode);
    kb.tick(cia);
    cia.write(Cia8520::kCra, 0);
    kb.tick(cia);
    EXPECT(decode(cia.read(Cia8520::kSdr)) == 0xE2);
    EXPECT(!kb.caps_lock());
}

// --- Through the bus ---------------------------------------------------------------

void test_cia_address_decoding() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();
    bus.write8(kCiaA + 0x200, 0x03);  // DDRA
    bus.write8(kCiaB + 0x300, 0xFF);  // DDRB
    EXPECT(m->cias().a().read(Cia8520::kDdra) == 0x03);
    EXPECT(m->cias().b().read(Cia8520::kDdrb) == 0xFF);
    EXPECT(bus.read8(kCiaA + 0x200) == 0x03);

    EXPECT_THROWS(bus.read8(0xBFE000), BusError);  // even address with A13 = 1: no CIA
    EXPECT_THROWS(bus.read8(0xBFD001), BusError);  // odd address with A12 = 1: no CIA
    EXPECT_THROWS(bus.read16(0xBFF000), BusError);  // A12 = A13 = 1: neither

    EXPECT(bus.read16(0xBFE200) == 0xFF03);  // word: CIA-B half not selected
    bus.write8(kCiaB + 0x200, 0x5A);         // CIA-B DDRA
    EXPECT(bus.read16(0xBFC200) == 0x5A03);  // A12 = A13 = 0: both CIAs
}

void test_overlay_follows_cia_a_pa0() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();
    std::vector<uint8_t> rom(MemoryBus::kRomSize, 0);
    rom[4] = 0xAB;
    bus.load_kickstart(rom);
    bus.chip_ram()[4] = 0x11;
    m->cias().reset();
    EXPECT(bus.overlay());  // PA0 is an input at reset: pulled high

    bus.write8(kCiaA + 0x200, 0x03);  // DDRA: PA0, PA1 outputs; PRA latch is 0 after reset
    EXPECT(!bus.overlay());           // so OVL goes low at once
    EXPECT(bus.read8(4) == 0x11);     // chip RAM visible at $000000

    bus.write8(kCiaA, 0x01);
    EXPECT(bus.read8(4) == 0xAB);
}

void test_mouse_inputs() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();

    EXPECT((bus.read8(kCiaA) & 0x40) != 0);  // /FIR0 high: released
    m->mouse_buttons(true, false, false);
    EXPECT((bus.read8(kCiaA) & 0x40) == 0);

    const uint32_t potgor = MemoryBus::kCustomBase + reg::kPotGoR;
    EXPECT((bus.read16(potgor) & Paula::kDatLy) != 0);
    m->mouse_buttons(false, true, true);
    EXPECT((bus.read16(potgor) & (Paula::kDatLy | Paula::kDatLx)) == 0);
    m->mouse_buttons(false, false, false);
    bus.write16(MemoryBus::kCustomBase + reg::kPotGo, 0x0800);  // OUTLY with DATLY = 0: driven low
    EXPECT((bus.read16(potgor) & Paula::kDatLy) == 0);

    const uint32_t joy0dat = MemoryBus::kCustomBase + reg::kJoy0Dat;
    m->mouse_move(3, -2);
    EXPECT(bus.read16(joy0dat) == 0xFE03);
    m->mouse_move(254, 3);  // counters wrap at 8 bits
    EXPECT(bus.read16(joy0dat) == 0x0101);
    bus.write16(MemoryBus::kCustomBase + reg::kJoyTest, 0xFCFC);
    EXPECT(bus.read16(joy0dat) == 0xFDFD);
}

// JOY1DAT: right = bit 1, left = bit 9, down = bit 1 ^ bit 0, up = bit 9 ^ bit 8.
uint16_t joy1(Machine& m, bool up, bool down, bool left, bool right) {
    m.joystick(up, down, left, right, false);
    return m.bus().read16(MemoryBus::kCustomBase + reg::kJoy1Dat);
}

void test_joystick_inputs() {
    auto m = std::make_unique<Machine>();
    EXPECT(joy1(*m, false, false, false, false) == 0x0000);
    EXPECT(joy1(*m, false, false, false, true) == 0x0003);   // right: X1, and X1 ^ X0 = 0
    EXPECT(joy1(*m, false, true, false, false) == 0x0001);   // down
    EXPECT(joy1(*m, false, false, true, false) == 0x0300);   // left: Y1, and Y1 ^ Y0 = 0
    EXPECT(joy1(*m, true, false, false, false) == 0x0100);   // up
    EXPECT(joy1(*m, true, false, false, true) == 0x0103);    // up + right
    EXPECT(joy1(*m, false, true, true, false) == 0x0301);    // down + left
    EXPECT(m->bus().read16(MemoryBus::kCustomBase + reg::kJoy0Dat) == 0);  // mouse port untouched

    EXPECT((m->bus().read8(kCiaA) & Cias::kPraFir1) != 0);  // /FIR1 released
    m->joystick(false, false, false, false, true);
    EXPECT((m->bus().read8(kCiaA) & Cias::kPraFir1) == 0);
    EXPECT((m->bus().read8(kCiaA) & Cias::kPraFir0) != 0);  // left mouse button unaffected
}

// One frame: 71051 color clocks = 14210 E clocks, 313 HSYNCs, 1 VSYNC.
void test_e_clock_rate_and_tod_sources() {
    auto m = std::make_unique<Machine>();
    Cia8520& a = m->cias().a();
    a.write(Cia8520::kTaLo, 0xE7);  // latch 999: period 1000 E clocks
    a.write(Cia8520::kTaHi, 0x03);
    a.write(Cia8520::kCra, Cia8520::kCrStart);
    m->run_frame();
    EXPECT(a.timer_a() == 999 - 210);  // 14210 = 14 * 1000 + 210 ticks
    EXPECT(a.tod() == 1);
    EXPECT(m->cias().b().tod() == 313);
}

void test_timer_interrupts_reach_the_cpu_levels() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();
    bus.write16(MemoryBus::kCustomBase + reg::kIntEna,
                reg::kSetClr | reg::kIntEn | reg::kIntPorts | reg::kIntExter);

    // CIA-A timer A -> INT2 (PORTS, level 2).
    bus.write8(kCiaA + 0xD00, Cia8520::kIcrIr | Cia8520::kIcrTa);  // ICR
    bus.write8(kCiaA + 0x400, 100);                                   // TALO
    bus.write8(kCiaA + 0x500, 0);                                     // TAHI
    bus.write8(kCiaA + 0xE00, Cia8520::kCrStart);                     // CRA
    m->run_frame();
    EXPECT(m->chipset().interrupt_level() == 2);

    // Acknowledge: read ICR, then clear INTREQ. Stop the timer first.
    bus.write8(kCiaA + 0xE00, 0);
    EXPECT(bus.read8(kCiaA + 0xD00) == (Cia8520::kIcrIr | Cia8520::kIcrTa));
    bus.write16(MemoryBus::kCustomBase + reg::kIntReq, reg::kIntPorts);
    EXPECT(m->chipset().interrupt_level() == 0);

    // CIA-B timer B, one-shot -> INT6 (EXTER, level 6).
    bus.write8(kCiaB + 0xD00, Cia8520::kIcrIr | Cia8520::kIcrTb);
    bus.write8(kCiaB + 0xF00, Cia8520::kCrRunMode);  // CRB: one-shot
    bus.write8(kCiaB + 0x600, 50);
    bus.write8(kCiaB + 0x700, 0);  // starts it
    m->run_frame();
    EXPECT(m->chipset().interrupt_level() == 6);
}

void test_key_press_raises_level_2() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();
    bus.write16(MemoryBus::kCustomBase + reg::kIntEna, reg::kSetClr | reg::kIntEn | reg::kIntPorts);
    bus.write8(kCiaA + 0xD00, Cia8520::kIcrIr | Cia8520::kIcrSp);

    m->key_event(0x20, true);  // 'A'
    m->run_frame();
    EXPECT(m->chipset().interrupt_level() == 2);
    EXPECT(decode(bus.read8(kCiaA + 0xC00)) == 0x20);  // SDR
}

}  // namespace

void run_cia_tests() {
    test_ports_and_data_direction();
    test_timer_continuous();
    test_timer_interrupt_mask();
    test_timer_one_shot();
    test_timer_force_load();
    test_timer_b_counts_timer_a_underflows();
    test_time_of_day();
    test_serial_input();
    test_keyboard_encoding();
    test_keyboard_handshake();
    test_keyboard_caps_lock_toggles();
    test_cia_address_decoding();
    test_overlay_follows_cia_a_pa0();
    test_mouse_inputs();
    test_joystick_inputs();
    test_e_clock_rate_and_tod_sources();
    test_timer_interrupts_reach_the_cpu_levels();
    test_key_press_raises_level_2();
}
