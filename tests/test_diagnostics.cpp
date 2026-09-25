#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>

#include "core/custom_registers.hpp"
#include "core/debug_dump.hpp"
#include "core/machine.hpp"
#include "test_framework.hpp"

namespace {

using amiga::Machine;
using amiga::MemoryBus;
namespace reg = amiga::reg;

void poke(MemoryBus& bus, uint32_t address, std::initializer_list<uint16_t> words) {
    for (const uint16_t w : words) {
        bus.write16(address, w);
        address += 2;
    }
}

// A machine running `program` at $1000 (no Kickstart: vectors in chip RAM).
std::unique_ptr<Machine> machine_with(std::initializer_list<uint16_t> program) {
    auto m = std::make_unique<Machine>();
    poke(m->bus(), 0x0000, {0x0008, 0x0000, 0x0000, 0x1000});  // SSP $80000, PC $1000
    poke(m->bus(), 0x1000, program);
    m->reset();
    return m;
}

std::string report(Machine& m, const Machine::Deadlock& d) {
    std::FILE* file = std::tmpfile();
    amiga::dump_deadlock(m, d, file);
    std::rewind(file);
    std::string text;
    for (int c = std::fgetc(file); c != EOF; c = std::fgetc(file)) text += static_cast<char>(c);
    std::fclose(file);
    return text;
}

void test_single_instruction_loop() {
    auto m = machine_with({0x60FE});  // BRA.S *
    std::optional<Machine::Deadlock> found;
    for (int f = 0; f < 20 && !found; ++f) {
        m->run_frame();
        found = m->take_deadlock();
    }
    EXPECT(found.has_value());
    EXPECT(found && found->pc_a == 0x1000 && found->pc_b == 0x1000);
    for (int f = 0; f < 20; ++f) {  // reported once per loop
        m->run_frame();
        EXPECT(!m->take_deadlock().has_value());
    }
}

// "Wait for the left mouse button": the report names the register and why.
void test_two_instruction_loop_report() {
    auto m = machine_with({
        0x0839, 0x0006, 0x00BF, 0xE001,  // $1000 BTST #6,$BFE001
        0x66F6,                          // $1008 BNE.S $1000
    });
    std::optional<Machine::Deadlock> found;
    for (int f = 0; f < 60 && !found; ++f) {
        m->run_frame();
        found = m->take_deadlock();
    }
    EXPECT(found.has_value());
    if (!found) return;
    const std::string text = report(*m, *found);
    EXPECT(text.find("CIA-A PRA") != std::string::npos);
    EXPECT(text.find("mouse/fire button") != std::string::npos);
    EXPECT(text.find("DMACONR") != std::string::npos && text.find("INTREQ") != std::string::npos);
    EXPECT(text.find("BBUSY=0") != std::string::npos);

    m->mouse_buttons(true, false, false);  // the button it waits for: the loop ends
    m->run_frame();
    EXPECT(m->cpu().pc() > 0x1008);
}

// A vertical blank interrupt every frame must not hide the deadlock.
void test_interrupts_do_not_break_the_count() {
    auto m = machine_with({0x60FE});  // BRA.S *
    MemoryBus& bus = m->bus();
    poke(bus, 0x006C, {0x0000, 0x2000});  // level 3 autovector
    poke(bus, 0x2000, {0x33FC, 0x0020, 0x00DF, 0xF09C, 0x4E73});  // MOVE.W #$0020,INTREQ ; RTE
    bus.write16(MemoryBus::kCustomBase + reg::kIntEna, reg::kSetClr | reg::kIntEn | reg::kIntVertb);
    m->cpu().set_sr(0x2000);  // main loop at mask 0
    std::optional<Machine::Deadlock> found;
    for (int f = 0; f < 20 && !found; ++f) {
        m->run_frame();
        found = m->take_deadlock();
    }
    EXPECT(found.has_value());
    EXPECT(m->cpu().exception_count(27) >= 2);  // the handler did run during the count (~3.5 frames)
}

// Counting and copying loops change registers each pass: progress, not a deadlock.
void test_progress_loops_are_not_flagged() {
    auto count = machine_with({0x203C, 0x0002, 0x0000,  // MOVE.L #$20000,D0 (131072 passes)
                               0x5380, 0x6EFC,          // SUBQ.L #1,D0 ; BGT.S (Kickstart's delay loop)
                               0x60FE});                // BRA.S *
    auto copy = machine_with({0x41F9, 0x0002, 0x0000, 0x303C, 0xFFFF,  // LEA $20000,A0 ; MOVE.W #$FFFF,D0
                              0x20C2, 0x51C8, 0xFFFC,                  // MOVE.L D2,(A0)+ ; DBRA D0
                              0x60FE});
    for (auto* m : {count.get(), copy.get()}) {
        std::optional<Machine::Deadlock> found;
        for (int f = 0; f < 60 && !found; ++f) {
            m->run_frame();
            found = m->take_deadlock();
        }
        // Only the final BRA.S * is reported, not the counting/copying loop.
        EXPECT(found.has_value() && found->pc_a == found->pc_b);
        EXPECT(found && m->cpu().pc() == found->pc_a);
    }
}

// Prince of Persia's loader: a loop at interrupt mask 0, then the program
// raises its own mask (MOVE #$2700,SR) and waits: still a deadlock to report.
void test_loop_after_raising_the_mask() {
    auto m = machine_with({
        0x46FC, 0x2000,  // MOVE #$2000,SR   (mask 0)
        0x7064,          // MOVEQ #100,D0
        0x51C8, 0xFFFE,  // DBRA D0,*        (a short loop at mask 0)
        0x46FC, 0x2700,  // MOVE #$2700,SR   (mask 7, not an interrupt)
        0x60FE,          // BRA.S *
    });
    std::optional<Machine::Deadlock> found;
    for (int f = 0; f < 30 && !found; ++f) {
        m->run_frame();
        found = m->take_deadlock();
    }
    EXPECT(found.has_value() && found->pc_a == 0x100E);  // the BRA.S *
}

void test_longer_loops_are_not_flagged() {
    auto m = machine_with({0x4E71, 0x4E71, 0x60FA});  // NOP ; NOP ; BRA.S (3 instructions)
    for (int f = 0; f < 20; ++f) {
        m->run_frame();
        EXPECT(!m->take_deadlock().has_value());
    }
}

void test_trace_records_opcodes() {
    auto m = machine_with({0x7001, 0x7202, 0x60FE});  // MOVEQ #1,D0 ; MOVEQ #2,D1 ; BRA.S *
    EXPECT(m->trace_enabled());  // on by default
    m->run_frame();
    m->bus().write16(0x1004, 0x4E71);  // overwrite the loop afterwards
    EXPECT(m->trace(0) != nullptr && m->trace(0)->pc == 0x1004);
    EXPECT(m->trace(0)->opcode == 0x60FE);  // the trace keeps what actually ran
}

void test_slow_ram_toggle() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();
    bus.write16(0xC00000, 0x1234);
    EXPECT(bus.read16(0xC00000) == 0x1234);  // trapdoor RAM (default)

    m->set_slow_ram(false);  // stock 512KB A500: custom chips answer there
    bus.write16(MemoryBus::kCustomBase + reg::kIntEna, reg::kSetClr | reg::kIntEn);
    EXPECT(bus.read16(0xC00000 + reg::kIntEnaR) == reg::kIntEn);
    EXPECT(bus.read16(0xC7F000 + reg::kIntEnaR) == reg::kIntEn);

    m->set_slow_ram(true);
    EXPECT(bus.read16(0xC00000) == 0x1234);  // contents kept
}

}  // namespace

void run_diagnostics_tests() {
    test_single_instruction_loop();
    test_two_instruction_loop_report();
    test_interrupts_do_not_break_the_count();
    test_progress_loops_are_not_flagged();
    test_loop_after_raising_the_mask();
    test_longer_loops_are_not_flagged();
    test_trace_records_opcodes();
    test_slow_ram_toggle();
}
