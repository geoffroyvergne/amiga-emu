#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>

#include "core/cpu68000.hpp"
#include "core/custom_registers.hpp"
#include "core/machine.hpp"
#include "core/memory_bus.hpp"
#include "test_framework.hpp"

namespace {

using amiga::Cpu68000;
using amiga::CpuError;
using amiga::MemoryBus;

constexpr uint32_t kProgram = 0x1000;
constexpr uint32_t kStack = 0x8000;
constexpr uint16_t kC = Cpu68000::kFlagC;
constexpr uint16_t kV = Cpu68000::kFlagV;
constexpr uint16_t kZ = Cpu68000::kFlagZ;
constexpr uint16_t kN = Cpu68000::kFlagN;
constexpr uint16_t kX = Cpu68000::kFlagX;

struct Fixture {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    Cpu68000 cpu{*bus};

    explicit Fixture(std::initializer_list<uint16_t> program) {
        bus->set_overlay(false);
        poke(kProgram, program);
        cpu.set_pc(kProgram);
        cpu.set_a(7, kStack);
    }

    void poke(uint32_t address, std::initializer_list<uint16_t> words) {
        for (const uint16_t w : words) {
            bus->write16(address, w);
            address += 2;
        }
    }

    [[nodiscard]] uint16_t ccr() const { return cpu.sr() & (kX | kN | kZ | kV | kC); }
    void set_ccr(uint16_t flags) { cpu.set_sr(static_cast<uint16_t>(0x2700 | flags)); }
};

bool takes_illegal_exception(std::initializer_list<uint16_t> program) {
    Fixture f{program};
    f.bus->write32(4 * 4, 0x4000);  // illegal instruction vector
    f.cpu.step();
    return f.cpu.pc() == 0x4000;
}

// --- MOVE: every addressing mode ------------------------------------------------

void test_move_immediate_to_register() {
    Fixture f{0x303C, 0x8000};  // MOVE.W #$8000,D0
    f.cpu.set_d(0, 0x1234'0000);
    EXPECT(f.cpu.step() == 8);
    EXPECT(f.cpu.d(0) == 0x1234'8000);
    EXPECT(f.ccr() == kN);
    EXPECT(f.cpu.pc() == kProgram + 4);
}

void test_move_long_immediate_to_indirect() {
    Fixture f{0x20BC, 0x1234, 0x5678};  // MOVE.L #$12345678,(A0)
    f.cpu.set_a(0, 0x3000);
    EXPECT(f.cpu.step() == 20);
    EXPECT(f.bus->read32(0x3000) == 0x1234'5678);
}

void test_move_postincrement_both_sides() {
    Fixture f{0x12D8};  // MOVE.B (A0)+,(A1)+
    f.bus->write8(0x3000, 0xAB);
    f.cpu.set_a(0, 0x3000);
    f.cpu.set_a(1, 0x3100);
    EXPECT(f.cpu.step() == 12);
    EXPECT(f.bus->read8(0x3100) == 0xAB);
    EXPECT(f.cpu.a(0) == 0x3001);
    EXPECT(f.cpu.a(1) == 0x3101);
    EXPECT(f.ccr() == kN);
}

void test_move_byte_predecrement_a7_stays_aligned() {
    Fixture f{0x1F00};  // MOVE.B D0,-(A7)
    f.cpu.set_d(0, 0x5A);
    EXPECT(f.cpu.step() == 8);
    EXPECT(f.cpu.a(7) == kStack - 2);
    EXPECT(f.bus->read8(kStack - 2) == 0x5A);
}

void test_move_predecrement_source() {
    Fixture f{0x3220};  // MOVE.W -(A0),D1
    f.bus->write16(0x3000, 0x1234);
    f.cpu.set_a(0, 0x3002);
    EXPECT(f.cpu.step() == 10);
    EXPECT(f.cpu.d(1) == 0x1234);
    EXPECT(f.cpu.a(0) == 0x3000);
}

void test_move_displacement_to_indexed() {
    // MOVE.L 16(A0),4(A1,D0.W): source extension word comes first.
    Fixture f{0x23A8, 0x0010, 0x0004};
    f.bus->write32(0x3010, 0xCAFE'BABE);
    f.cpu.set_a(0, 0x3000);
    f.cpu.set_a(1, 0x3200);
    f.cpu.set_d(0, 0x0000'FFFE);  // .W index = -2
    EXPECT(f.cpu.step() == 30);
    EXPECT(f.bus->read32(0x3202) == 0xCAFE'BABE);
    EXPECT(f.cpu.pc() == kProgram + 6);
}

void test_move_absolute_short_to_absolute_long() {
    Fixture f{0x33F8, 0x3000, 0x0000, 0x3100};  // MOVE.W $3000.W,$00003100.L
    f.bus->write16(0x3000, 0xBEEF);
    EXPECT(f.cpu.step() == 24);
    EXPECT(f.bus->read16(0x3100) == 0xBEEF);
}

void test_move_pc_relative() {
    Fixture f{0x303A, 0x0010};  // MOVE.W 16(PC),D0: base = extension word address
    f.bus->write16(kProgram + 2 + 0x10, 0x7777);
    EXPECT(f.cpu.step() == 12);
    EXPECT(f.cpu.d(0) == 0x7777);

    Fixture x{0x243B, 0x1808};  // MOVE.L 8(PC,D1.L),D2
    x.cpu.set_d(1, 0x100);
    x.bus->write32(kProgram + 2 + 0x100 + 8, 0x0BAD'F00D);
    EXPECT(x.cpu.step() == 18);
    EXPECT(x.cpu.d(2) == 0x0BAD'F00D);
}

void test_movea_immediate_keeps_flags() {
    Fixture f{0x2A7C, 0x00DF, 0xF000};  // MOVEA.L #$DFF000,A5
    f.set_ccr(kZ | kC);
    EXPECT(f.cpu.step() == 12);
    EXPECT(f.cpu.a(5) == 0x00DF'F000);
    EXPECT(f.ccr() == (kZ | kC));
}

void test_move_odd_word_access_is_address_error() {
    Fixture f{0x3010};  // MOVE.W (A0),D0
    f.cpu.set_a(0, 0x3001);
    try {
        f.cpu.step();
        EXPECT(false);
    } catch (const CpuError& e) {
        EXPECT(e.kind() == CpuError::Kind::OddDataAccess);
        EXPECT(e.address() == 0x3001);
        EXPECT(e.pc() == kProgram);
    }

    Fixture byte{0x1010};  // MOVE.B (A0),D0: bytes may be odd
    byte.cpu.set_a(0, 0x3001);
    byte.cpu.step();
}

// --- TST / CMP ------------------------------------------------------------------

void test_tst() {
    Fixture f{0x4A80};  // TST.L D0
    f.set_ccr(kX | kV | kC);
    EXPECT(f.cpu.step() == 4);
    EXPECT(f.ccr() == (kX | kZ));

    Fixture m{0x4A10};  // TST.B (A0)
    m.bus->write8(0x3000, 0x80);
    m.cpu.set_a(0, 0x3000);
    EXPECT(m.cpu.step() == 8);
    EXPECT(m.ccr() == kN);

    EXPECT(takes_illegal_exception({0x4A48}));          // TST.W A0: 68020+
    EXPECT(takes_illegal_exception({0x4A7C, 0x0000}));  // TST.W #imm: 68020+
}

void test_cmp_flags() {
    Fixture f{0xB041};  // CMP.W D1,D0 -> D0 - D1
    f.cpu.set_d(0, 5);
    f.cpu.set_d(1, 7);
    f.set_ccr(kX);
    EXPECT(f.cpu.step() == 4);
    EXPECT(f.ccr() == (kX | kN | kC));  // X unaffected
    EXPECT(f.cpu.d(0) == 5);            // compare only

    Fixture v{0xB081};  // CMP.L D1,D0
    v.cpu.set_d(0, 0x8000'0000);
    v.cpu.set_d(1, 1);
    EXPECT(v.cpu.step() == 6);
    EXPECT(v.ccr() == kV);

    Fixture m{0xB010};  // CMP.B (A0),D0
    m.bus->write8(0x3000, 0x42);
    m.cpu.set_a(0, 0x3000);
    m.cpu.set_d(0, 0xFFFF'FF42);
    EXPECT(m.cpu.step() == 8);
    EXPECT(m.ccr() == kZ);
}

void test_cmpa() {
    Fixture w{0xB0C1};  // CMPA.W D1,A0: source sign-extended, 32-bit compare
    w.cpu.set_a(0, 0xFFFF'FFFF);
    w.cpu.set_d(1, 0x0000'FFFF);
    EXPECT(w.cpu.step() == 6);
    EXPECT(w.ccr() == kZ);

    Fixture l{0xB1C1};  // CMPA.L D1,A0
    l.cpu.set_a(0, 1);
    l.cpu.set_d(1, 2);
    l.cpu.step();
    EXPECT(l.ccr() == (kN | kC));
}

void test_cmpi_and_cmpm() {
    Fixture b{0x0C10, 0x0010};  // CMPI.B #$10,(A0)
    b.bus->write8(0x3000, 0x10);
    b.cpu.set_a(0, 0x3000);
    EXPECT(b.cpu.step() == 12);
    EXPECT(b.ccr() == kZ);

    Fixture l{0x0C80, 0x0000, 0x0001};  // CMPI.L #1,D0
    EXPECT(l.cpu.step() == 14);
    EXPECT(l.ccr() == (kN | kC));  // 0 - 1

    Fixture m{0xB388};  // CMPM.L (A0)+,(A1)+
    m.bus->write32(0x3000, 0x1234'5678);
    m.bus->write32(0x3100, 0x1234'5678);
    m.cpu.set_a(0, 0x3000);
    m.cpu.set_a(1, 0x3100);
    EXPECT(m.cpu.step() == 20);
    EXPECT(m.ccr() == kZ);
    EXPECT(m.cpu.a(0) == 0x3004);
    EXPECT(m.cpu.a(1) == 0x3104);
}

// --- Branches -------------------------------------------------------------------

void test_bra() {
    Fixture s{0x6004};  // BRA.S *+6
    EXPECT(s.cpu.step() == 10);
    EXPECT(s.cpu.pc() == kProgram + 2 + 4);

    Fixture w{0x6000, 0x0100};  // BRA.W: base is the address of the displacement word
    EXPECT(w.cpu.step() == 10);
    EXPECT(w.cpu.pc() == kProgram + 2 + 0x100);

    Fixture back{0x60FE};  // BRA.S * (infinite loop)
    back.cpu.step();
    EXPECT(back.cpu.pc() == kProgram);
}

void test_bcc_not_taken_timing() {
    Fixture s{0x6704};  // BEQ.S with Z clear
    EXPECT(s.cpu.step() == 8);
    EXPECT(s.cpu.pc() == kProgram + 2);

    Fixture w{0x6700, 0x0100};  // BEQ.W with Z clear
    EXPECT(w.cpu.step() == 12);
    EXPECT(w.cpu.pc() == kProgram + 4);
}

// CMP.W D1,D0 then Bcc.S: checks the condition logic with real comparisons.
bool branch_after_compare(uint16_t d0, uint16_t d1, unsigned cc) {
    Fixture f{0xB041, static_cast<uint16_t>(0x6002 | cc << 8)};
    f.cpu.set_d(0, d0);
    f.cpu.set_d(1, d1);
    f.cpu.step();
    f.cpu.step();
    return f.cpu.pc() == kProgram + 6;
}

void test_conditions() {
    constexpr unsigned HI = 2, LS = 3, CC = 4, CS = 5, NE = 6, EQ = 7, GE = 0xC, LT = 0xD, GT = 0xE,
                       LE = 0xF;
    // 5 vs 7: lower both signed and unsigned.
    EXPECT(branch_after_compare(5, 7, LT) && branch_after_compare(5, 7, LE));
    EXPECT(!branch_after_compare(5, 7, GT) && !branch_after_compare(5, 7, GE));
    EXPECT(branch_after_compare(5, 7, CS) && branch_after_compare(5, 7, LS));
    EXPECT(!branch_after_compare(5, 7, HI) && !branch_after_compare(5, 7, CC));
    // -1 vs 1: lower signed, higher unsigned.
    EXPECT(branch_after_compare(0xFFFF, 1, LT) && branch_after_compare(0xFFFF, 1, HI));
    // $8000 vs 1: overflow case, -32768 < 1 signed.
    EXPECT(branch_after_compare(0x8000, 1, LT) && !branch_after_compare(0x8000, 1, GE));
    // Equal.
    EXPECT(branch_after_compare(9, 9, EQ) && branch_after_compare(9, 9, GE) &&
           branch_after_compare(9, 9, LE) && branch_after_compare(9, 9, LS));
    EXPECT(!branch_after_compare(9, 9, NE) && !branch_after_compare(9, 9, GT) &&
           !branch_after_compare(9, 9, HI));
}

void test_bsr_rts() {
    Fixture f{0x6104, 0x60FE, 0x0000, 0x4E75};  // BSR.S sub ; BRA.S * ; dc.w 0 ; sub: RTS
    EXPECT(f.cpu.step() == 18);
    EXPECT(f.cpu.pc() == kProgram + 6);
    EXPECT(f.cpu.a(7) == kStack - 4);
    EXPECT(f.bus->read32(kStack - 4) == kProgram + 2);
    EXPECT(f.cpu.step() == 16);  // RTS
    EXPECT(f.cpu.pc() == kProgram + 2);
    EXPECT(f.cpu.a(7) == kStack);

    Fixture w{0x6100, 0x0010};  // BSR.W: returns after the displacement word
    w.cpu.step();
    EXPECT(w.cpu.pc() == kProgram + 2 + 0x10);
    EXPECT(w.bus->read32(kStack - 4) == kProgram + 4);
}

void test_jsr() {
    Fixture f{0x4E90};  // JSR (A0)
    f.cpu.set_a(0, 0x2000);
    EXPECT(f.cpu.step() == 16);
    EXPECT(f.cpu.pc() == 0x2000);
    EXPECT(f.bus->read32(kStack - 4) == kProgram + 2);

    Fixture l{0x4EB9, 0x0000, 0x2000};  // JSR $2000.L
    EXPECT(l.cpu.step() == 20);
    EXPECT(l.bus->read32(kStack - 4) == kProgram + 6);
}

void test_branch_to_odd_address() {
    Fixture f{0x60FF};  // BRA.S with displacement -1 (68000: no 32-bit form)
    f.cpu.step();
    EXPECT(f.cpu.pc() == kProgram + 1);
    EXPECT_THROWS(f.cpu.step(), CpuError);
}

// --- Bit manipulation -------------------------------------------------------------

void test_btst() {
    Fixture s{0x0800, 0x0003};  // BTST #3,D0
    s.cpu.set_d(0, 0x08);
    s.set_ccr(kN | kC);
    EXPECT(s.cpu.step() == 10);
    EXPECT(s.ccr() == (kN | kC));  // bit set -> Z clear; other flags untouched

    Fixture mod32{0x0800, 0x0023};  // BTST #35,D0 -> bit 3
    mod32.cpu.step();
    EXPECT(mod32.ccr() == kZ);

    Fixture m{0x0310};  // BTST D1,(A0): byte, bit number mod 8
    m.bus->write8(0x3000, 0x04);
    m.cpu.set_a(0, 0x3000);
    m.cpu.set_d(1, 10);  // -> bit 2
    EXPECT(m.cpu.step() == 8);
    EXPECT(m.ccr() == 0);

    Fixture imm{0x013C, 0x00F0};  // BTST D0,#$F0
    imm.cpu.set_d(0, 4);
    imm.cpu.step();
    EXPECT(imm.ccr() == 0);
}

void test_bset_bclr_bchg() {
    Fixture set{0x08D0, 0x0007};  // BSET #7,(A0)
    set.bus->write8(0x3000, 0x01);
    set.cpu.set_a(0, 0x3000);
    EXPECT(set.cpu.step() == 16);
    EXPECT(set.bus->read8(0x3000) == 0x81);
    EXPECT(set.ccr() == kZ);  // Z reflects the bit before the change

    Fixture clr{0x0380};  // BCLR D1,D0
    clr.cpu.set_d(0, 0xFF);
    clr.cpu.set_d(1, 4);
    EXPECT(clr.cpu.step() == 8);  // bit < 16
    EXPECT(clr.cpu.d(0) == 0xEF);
    EXPECT(clr.ccr() == 0);

    Fixture clr_high{0x0380};
    clr_high.cpu.set_d(1, 20);
    EXPECT(clr_high.cpu.step() == 10);

    Fixture chg{0x0840, 0x0000};  // BCHG #0,D0
    chg.cpu.set_d(0, 1);
    EXPECT(chg.cpu.step() == 10);
    EXPECT(chg.cpu.d(0) == 0);
}

// --- ADD with memory operands -----------------------------------------------------

void test_add_memory_forms() {
    Fixture src{0xD050};  // ADD.W (A0),D0
    src.bus->write16(0x3000, 0x0001);
    src.cpu.set_a(0, 0x3000);
    src.cpu.set_d(0, 0xFFFF);
    EXPECT(src.cpu.step() == 8);
    EXPECT(src.cpu.d(0) == 0);
    EXPECT(src.ccr() == (kX | kZ | kC));

    Fixture dst{0xD190};  // ADD.L D0,(A0)
    dst.bus->write32(0x3000, 0x10);
    dst.cpu.set_a(0, 0x3000);
    dst.cpu.set_d(0, 0x20);
    EXPECT(dst.cpu.step() == 20);
    EXPECT(dst.bus->read32(0x3000) == 0x30);

    Fixture imm{0xD0BC, 0x0001, 0x0000};  // ADD.L #$10000,D0
    EXPECT(imm.cpu.step() == 16);
    EXPECT(imm.cpu.d(0) == 0x1'0000);
}

// --- Dispatch table -------------------------------------------------------------

void test_dispatch_table_boundaries() {
    EXPECT(Cpu68000::is_implemented(0x4E75));   // RTS
    EXPECT(Cpu68000::is_implemented(0x6100));   // BSR.W
    EXPECT(Cpu68000::is_implemented(0x33FC));   // MOVE.W #imm,abs.L
    EXPECT(Cpu68000::is_implemented(0x4E71));   // NOP
    EXPECT(!Cpu68000::is_implemented(0x4AFC));  // ILLEGAL
    EXPECT(Cpu68000::is_implemented(0x4AC0));   // TAS D0
    EXPECT(Cpu68000::is_implemented(0x0108));   // MOVEP (bit-op space, mode 001)
    EXPECT(Cpu68000::is_implemented(0xD181));   // ADDX
    EXPECT(Cpu68000::is_implemented(0xB141));   // EOR.W D0,D1 (CMP space, opmode 101)
    EXPECT(!Cpu68000::is_implemented(0x4E7A));  // MOVEC (68010+)
    EXPECT(!Cpu68000::is_implemented(0xA000));  // line A
    EXPECT(!Cpu68000::is_implemented(0xF000));  // line F
    EXPECT(!Cpu68000::is_implemented(0x0C3C));  // CMPI to immediate
    EXPECT(!Cpu68000::is_implemented(0x01FC));  // BSET D0,#imm
    EXPECT(Cpu68000::is_implemented(0x3E7C));   // MOVEA.W #imm,A7
}

// A small program driving the custom chips through the bus, as Kickstart would.
void test_program_writes_custom_registers() {
    auto m = std::make_unique<amiga::Machine>();
    MemoryBus& bus = m->bus();
    const uint16_t program[] = {
        0x2A7C, 0x00DF, 0xF000,  // $1000 MOVEA.L #$DFF000,A5
        0x3B7C, 0x0F00, 0x0180,  // $1006 MOVE.W #$0F00,$180(A5)   COLOR00
        0x6102,                  // $100C BSR.S $1010
        0x60FE,                  // $100E BRA.S *
        0x3B7C, 0x00F0, 0x0182,  // $1010 MOVE.W #$00F0,$182(A5)   COLOR01
        0x4E75,                  // $1016 RTS
    };
    bus.write32(0, 0x0008'0000);  // reset SSP
    bus.write32(4, 0x0000'1000);  // reset PC
    for (size_t i = 0; i < std::size(program); ++i) {
        bus.write16(0x1000 + static_cast<uint32_t>(i) * 2, program[i]);
    }

    m->reset();
    m->run_frame();
    EXPECT(!m->cpu_halted());
    EXPECT(m->chipset().denise().color(0) == 0x0F00);
    EXPECT(m->chipset().denise().color(1) == 0x00F0);
    EXPECT(m->cpu().pc() == 0x100E);
    EXPECT(m->cpu().a(7) == 0x8'0000);
}

}  // namespace

void run_cpu_instruction_tests() {
    test_move_immediate_to_register();
    test_move_long_immediate_to_indirect();
    test_move_postincrement_both_sides();
    test_move_byte_predecrement_a7_stays_aligned();
    test_move_predecrement_source();
    test_move_displacement_to_indexed();
    test_move_absolute_short_to_absolute_long();
    test_move_pc_relative();
    test_movea_immediate_keeps_flags();
    test_move_odd_word_access_is_address_error();
    test_tst();
    test_cmp_flags();
    test_cmpa();
    test_cmpi_and_cmpm();
    test_bra();
    test_bcc_not_taken_timing();
    test_conditions();
    test_bsr_rts();
    test_jsr();
    test_branch_to_odd_address();
    test_btst();
    test_bset_bclr_bchg();
    test_add_memory_forms();
    test_dispatch_table_boundaries();
    test_program_writes_custom_registers();
}
