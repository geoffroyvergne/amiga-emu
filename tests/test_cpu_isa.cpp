#include <cstdint>
#include <initializer_list>
#include <memory>

#include "core/cpu68000.hpp"
#include "core/memory_bus.hpp"
#include "test_framework.hpp"

namespace {

using amiga::Cpu68000;
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
        uint32_t address = kProgram;
        for (const uint16_t w : program) {
            bus->write16(address, w);
            address += 2;
        }
        cpu.set_pc(kProgram);
        cpu.set_a(7, kStack);
        for (uint32_t vector = 2; vector < 48; ++vector) bus->write32(vector * 4, 0x4000 + vector * 0x10);
    }

    [[nodiscard]] uint16_t ccr() const { return cpu.sr() & 0x1F; }
    void set_ccr(uint16_t flags) { cpu.set_sr(static_cast<uint16_t>(0x2700 | flags)); }
    [[nodiscard]] static uint32_t handler(uint32_t vector) { return 0x4000 + vector * 0x10; }
};

// --- Data movement ---------------------------------------------------------------

void test_moveq_lea_pea() {
    Fixture q{0x70FF};  // MOVEQ #-1,D0
    EXPECT(q.cpu.step() == 4);
    EXPECT(q.cpu.d(0) == 0xFFFF'FFFF);
    EXPECT(q.ccr() == kN);

    Fixture lea{0x41E8, 0x0010};  // LEA 16(A0),A0
    lea.cpu.set_a(0, 0x2000);
    EXPECT(lea.cpu.step() == 8);
    EXPECT(lea.cpu.a(0) == 0x2010);

    Fixture pea{0x4879, 0x00FC, 0x0000};  // PEA $FC0000
    EXPECT(pea.cpu.step() == 20);
    EXPECT(pea.bus->read32(kStack - 4) == 0x00FC'0000);
}

void test_movem() {
    Fixture store{0x48E7, 0xC0C0};  // MOVEM.L D0-D1/A0-A1,-(A7)  (mask reversed for -(An))
    store.cpu.set_d(0, 0xD0);
    store.cpu.set_d(1, 0xD1);
    store.cpu.set_a(0, 0xA0);
    store.cpu.set_a(1, 0xA1);
    EXPECT(store.cpu.step() == 8 + 4 * 8);
    EXPECT(store.cpu.a(7) == kStack - 16);
    EXPECT(store.bus->read32(kStack - 16) == 0xD0);  // lowest register at lowest address
    EXPECT(store.bus->read32(kStack - 12) == 0xD1);
    EXPECT(store.bus->read32(kStack - 4) == 0xA1);

    Fixture load{0x4CDF, 0x0303};  // MOVEM.L (A7)+,D0-D1/A0-A1
    for (uint32_t i = 0; i < 4; ++i) load.bus->write32(kStack + 4 * i, 0x100 + i);
    EXPECT(load.cpu.step() == 12 + 4 * 8);
    EXPECT(load.cpu.d(0) == 0x100 && load.cpu.d(1) == 0x101);
    EXPECT(load.cpu.a(0) == 0x102 && load.cpu.a(1) == 0x103);
    EXPECT(load.cpu.a(7) == kStack + 16);

    Fixture word{0x4C90, 0x0001};  // MOVEM.W (A0),D0: sign-extended to 32 bits
    word.bus->write16(0x3000, 0x8000);
    word.cpu.set_a(0, 0x3000);
    word.cpu.step();
    EXPECT(word.cpu.d(0) == 0xFFFF'8000);
}

void test_exg_swap_ext_link_unlk() {
    Fixture exg{0xC188};  // EXG D0,A0
    exg.cpu.set_d(0, 1);
    exg.cpu.set_a(0, 2);
    EXPECT(exg.cpu.step() == 6);
    EXPECT(exg.cpu.d(0) == 2 && exg.cpu.a(0) == 1);

    Fixture swap{0x4840};  // SWAP D0
    swap.cpu.set_d(0, 0x1234'8765);
    swap.cpu.step();
    EXPECT(swap.cpu.d(0) == 0x8765'1234);
    EXPECT(swap.ccr() == kN);

    Fixture ext{0x4880, 0x48C0};  // EXT.W D0 ; EXT.L D0
    ext.cpu.set_d(0, 0x1234'0080);
    ext.cpu.step();
    EXPECT(ext.cpu.d(0) == 0x1234'FF80);
    ext.cpu.step();
    EXPECT(ext.cpu.d(0) == 0xFFFF'FF80);

    Fixture link{0x4E56, 0xFFF8, 0x4E5E};  // LINK A6,#-8 ; UNLK A6
    link.cpu.set_a(6, 0xA6A6);
    EXPECT(link.cpu.step() == 16);
    EXPECT(link.cpu.a(6) == kStack - 4);
    EXPECT(link.cpu.a(7) == kStack - 12);
    EXPECT(link.bus->read32(kStack - 4) == 0xA6A6);
    EXPECT(link.cpu.step() == 12);
    EXPECT(link.cpu.a(6) == 0xA6A6 && link.cpu.a(7) == kStack);
}

void test_movep() {
    Fixture f{0x01C8, 0x0000, 0x0348, 0x0000};  // MOVEP.L D0,0(A0) ; MOVEP.L 0(A0),D1
    f.cpu.set_d(0, 0x1122'3344);
    f.cpu.set_a(0, 0x3000);
    EXPECT(f.cpu.step() == 24);
    EXPECT(f.bus->read8(0x3000) == 0x11 && f.bus->read8(0x3002) == 0x22);
    EXPECT(f.bus->read8(0x3004) == 0x33 && f.bus->read8(0x3006) == 0x44);
    EXPECT(f.bus->read8(0x3001) == 0x00);
    f.cpu.step();
    EXPECT(f.cpu.d(1) == 0x1122'3344);
}

// --- Arithmetic ---------------------------------------------------------------------

void test_sub_family() {
    Fixture sub{0x9041};  // SUB.W D1,D0
    sub.cpu.set_d(0, 3);
    sub.cpu.set_d(1, 5);
    EXPECT(sub.cpu.step() == 4);
    EXPECT(sub.cpu.d(0) == 0xFFFE);
    EXPECT(sub.ccr() == (kX | kN | kC));

    Fixture v{0x9081};  // SUB.L D1,D0: $80000000 - 1 overflows
    v.cpu.set_d(0, 0x8000'0000);
    v.cpu.set_d(1, 1);
    v.cpu.step();
    EXPECT(v.cpu.d(0) == 0x7FFF'FFFF);
    EXPECT(v.ccr() == kV);

    Fixture q{0x5380, 0x5388};  // SUBQ.L #1,D0 ; SUBQ.L #1,A0 (no flags)
    q.cpu.set_d(0, 1);
    q.cpu.set_a(0, 0);
    q.cpu.step();
    EXPECT(q.cpu.d(0) == 0 && q.ccr() == kZ);
    q.cpu.step();
    EXPECT(q.cpu.a(0) == 0xFFFF'FFFF && q.ccr() == kZ);

    Fixture i{0x0440, 0x0010};  // SUBI.W #$10,D0
    i.cpu.set_d(0, 0x10);
    EXPECT(i.cpu.step() == 8);
    EXPECT(i.ccr() == kZ);

    Fixture x{0x9181};  // SUBX.L D1,D0 with X: 0 - 0 - 1, Z only cleared
    x.set_ccr(kX | kZ);
    x.cpu.step();
    EXPECT(x.cpu.d(0) == 0xFFFF'FFFF);
    EXPECT(x.ccr() == (kX | kN | kC));
}

void test_neg_clr_not() {
    Fixture neg{0x4440};  // NEG.W D0
    neg.cpu.set_d(0, 1);
    neg.cpu.step();
    EXPECT(neg.cpu.d(0) == 0xFFFF);
    EXPECT(neg.ccr() == (kX | kN | kC));

    Fixture neg0{0x4440};  // NEG of 0: no carry
    neg0.cpu.step();
    EXPECT(neg0.ccr() == kZ);

    Fixture negmin{0x4480};  // NEG.L $80000000 overflows
    negmin.cpu.set_d(0, 0x8000'0000);
    negmin.cpu.step();
    EXPECT(negmin.ccr() == (kX | kN | kV | kC));

    Fixture clr{0x4290};  // CLR.L (A0)
    clr.bus->write32(0x3000, 0xFFFF'FFFF);
    clr.cpu.set_a(0, 0x3000);
    EXPECT(clr.cpu.step() == 20);
    EXPECT(clr.bus->read32(0x3000) == 0 && clr.ccr() == kZ);

    Fixture not_{0x4600};  // NOT.B D0
    not_.cpu.set_d(0, 0x1234'5600);
    not_.cpu.step();
    EXPECT(not_.cpu.d(0) == 0x1234'56FF && not_.ccr() == kN);
}

void test_mul_div() {
    Fixture mulu{0xC0C1};  // MULU D1,D0
    mulu.cpu.set_d(0, 0xFFFF'FFFF);  // only the low word counts
    mulu.cpu.set_d(1, 0xFFFF);
    EXPECT(mulu.cpu.step() == 38 + 2 * 16);
    EXPECT(mulu.cpu.d(0) == 0xFFFE'0001);

    Fixture muls{0xC1C1};  // MULS D1,D0: -2 * 3
    muls.cpu.set_d(0, 0xFFFE);
    muls.cpu.set_d(1, 3);
    muls.cpu.step();
    EXPECT(muls.cpu.d(0) == 0xFFFF'FFFA && muls.ccr() == kN);

    Fixture divu{0x80C1};  // DIVU D1,D0: 100003 / 10
    divu.cpu.set_d(0, 100003);
    divu.cpu.set_d(1, 10);
    divu.cpu.step();
    EXPECT(divu.cpu.d(0) == (3u << 16 | 10000));

    Fixture overflow{0x80C1};  // quotient > $FFFF: V set, Dn unchanged
    overflow.cpu.set_d(0, 0x0010'0000);
    overflow.cpu.set_d(1, 1);
    overflow.cpu.step();
    EXPECT(overflow.cpu.d(0) == 0x0010'0000 && (overflow.ccr() & kV) != 0);

    Fixture divs{0x81C1};  // DIVS D1,D0: -7 / 2 = -3 remainder -1
    divs.cpu.set_d(0, 0xFFFF'FFF9);
    divs.cpu.set_d(1, 2);
    divs.cpu.step();
    EXPECT(divs.cpu.d(0) == 0xFFFF'FFFD);

    Fixture zero{0x80C1};  // divide by zero: vector 5, PC of the next instruction
    zero.cpu.step();
    EXPECT(zero.cpu.pc() == Fixture::handler(5));
    EXPECT(zero.bus->read32(kStack - 4) == kProgram + 2);
}

void test_bcd() {
    Fixture abcd{0xC101};  // ABCD D1,D0: 19 + 23 + X(1) = 43
    abcd.cpu.set_d(0, 0x19);
    abcd.cpu.set_d(1, 0x23);
    abcd.set_ccr(kX | kZ);
    abcd.cpu.step();
    EXPECT((abcd.cpu.d(0) & 0xFF) == 0x43);
    EXPECT((abcd.ccr() & (kX | kC | kZ)) == 0);

    Fixture carry{0xC101};  // 99 + 01 = 00, carry
    carry.cpu.set_d(0, 0x99);
    carry.cpu.set_d(1, 0x01);
    carry.cpu.step();
    EXPECT((carry.cpu.d(0) & 0xFF) == 0x00 && (carry.ccr() & (kX | kC)) == (kX | kC));

    Fixture sbcd{0x8101};  // SBCD D1,D0: 10 - 01 = 09
    sbcd.cpu.set_d(0, 0x10);
    sbcd.cpu.set_d(1, 0x01);
    sbcd.cpu.step();
    EXPECT((sbcd.cpu.d(0) & 0xFF) == 0x09);

    Fixture nbcd{0x4800};  // NBCD D0: 0 - 01 = 99, borrow
    nbcd.cpu.set_d(0, 0x01);
    nbcd.cpu.step();
    EXPECT((nbcd.cpu.d(0) & 0xFF) == 0x99 && (nbcd.ccr() & kC) != 0);
}

// --- Logic and shifts ---------------------------------------------------------------

void test_logic() {
    Fixture f{0xC041, 0x8041, 0xB340};  // AND.W D1,D0 ; OR.W D1,D0 ; EOR.W D1,D0
    f.cpu.set_d(0, 0xF0F0);
    f.cpu.set_d(1, 0xFF00);
    f.cpu.step();
    EXPECT(f.cpu.d(0) == 0xF000 && f.ccr() == kN);
    f.cpu.step();
    EXPECT(f.cpu.d(0) == 0xFF00);
    f.cpu.step();
    EXPECT(f.cpu.d(0) == 0 && f.ccr() == kZ);

    Fixture andi{0x0280, 0x0000, 0x00FF};  // ANDI.L #$FF,D0
    andi.cpu.set_d(0, 0x1234'5678);
    EXPECT(andi.cpu.step() == 14);
    EXPECT(andi.cpu.d(0) == 0x78);

    Fixture tas{0x4AD0};  // TAS (A0)
    tas.cpu.set_a(0, 0x3000);
    tas.cpu.step();
    EXPECT(tas.bus->read8(0x3000) == 0x80 && tas.ccr() == kZ);
}

void test_shifts() {
    Fixture asl{0xE340};  // ASL.W #1,D0: sign change sets V
    asl.cpu.set_d(0, 0x4000);
    EXPECT(asl.cpu.step() == 8);
    EXPECT(asl.cpu.d(0) == 0x8000 && asl.ccr() == (kN | kV));

    Fixture asr{0xE440};  // ASR.W #2,D0 keeps the sign
    asr.cpu.set_d(0, 0x8003);
    asr.cpu.step();
    EXPECT(asr.cpu.d(0) == 0xE000 && asr.ccr() == (kX | kN | kC));

    Fixture lsr{0xE288};  // LSR.L #1,D0
    lsr.cpu.set_d(0, 0x8000'0001);
    EXPECT(lsr.cpu.step() == 10);
    EXPECT(lsr.cpu.d(0) == 0x4000'0000 && lsr.ccr() == (kX | kC));

    Fixture rol{0xE318};  // ROL.B #1,D0: X unaffected
    rol.cpu.set_d(0, 0x81);
    rol.set_ccr(kX);
    rol.cpu.step();
    EXPECT((rol.cpu.d(0) & 0xFF) == 0x03 && rol.ccr() == (kX | kC));

    Fixture roxl{0xE310};  // ROXL.B #1,D0: X shifted in
    roxl.cpu.set_d(0, 0x80);
    roxl.set_ccr(kX);
    roxl.cpu.step();
    EXPECT((roxl.cpu.d(0) & 0xFF) == 0x01 && roxl.ccr() == (kX | kC));

    Fixture zero{0xE3A8};  // LSL.L D1,D0 with D1 = 0: C cleared, X kept
    zero.cpu.set_d(0, 5);
    zero.set_ccr(kX | kC);
    EXPECT(zero.cpu.step() == 8);
    EXPECT(zero.cpu.d(0) == 5 && zero.ccr() == kX);

    Fixture eight{0xE108};  // LSL.B #8,D0 (count field 0 = 8)
    eight.cpu.set_d(0, 0x1234'5601);
    eight.cpu.step();
    EXPECT(eight.cpu.d(0) == 0x1234'5600 && eight.ccr() == (kX | kC | kZ));

    Fixture mem{0xE3D0};  // LSL.W (A0)
    mem.bus->write16(0x3000, 0xC000);
    mem.cpu.set_a(0, 0x3000);
    mem.cpu.step();
    EXPECT(mem.bus->read16(0x3000) == 0x8000);
}

// --- Flow control and system --------------------------------------------------------

void test_dbcc_and_scc() {
    Fixture f{0x51C8, 0xFFFE};  // DBF D0,* : loops D0 + 1 times
    f.cpu.set_d(0, 0xFFFF'0002);
    EXPECT(f.cpu.step() == 10 && f.cpu.pc() == kProgram);
    f.cpu.step();
    f.cpu.step();
    EXPECT(f.cpu.pc() == kProgram + 4);  // expired
    EXPECT(f.cpu.d(0) == 0xFFFF'FFFF);

    Fixture t{0x57C8, 0xFFFE};  // DBEQ with Z set: no decrement
    t.set_ccr(kZ);
    EXPECT(t.cpu.step() == 12 && t.cpu.pc() == kProgram + 4 && t.cpu.d(0) == 0);

    Fixture scc{0x57C0, 0x56C1};  // SEQ D0 ; SNE D1
    scc.set_ccr(kZ);
    EXPECT(scc.cpu.step() == 6);
    EXPECT((scc.cpu.d(0) & 0xFF) == 0xFF);
    scc.cpu.step();
    EXPECT((scc.cpu.d(1) & 0xFF) == 0x00);
}

void test_traps() {
    Fixture trap{0x4E4F};  // TRAP #15
    trap.cpu.set_sr(0x0000);  // from user mode
    trap.cpu.set_a(7, 0x6000);
    EXPECT(trap.cpu.step() == 34);
    EXPECT(trap.cpu.pc() == Fixture::handler(47));
    EXPECT(trap.cpu.supervisor());
    EXPECT(trap.cpu.usp() == 0x6000);
    EXPECT(trap.bus->read32(kStack - 4) == kProgram + 2);

    Fixture chk{0x4181};  // CHK D1,D0
    chk.cpu.set_d(0, 11);
    chk.cpu.set_d(1, 10);
    chk.cpu.step();
    EXPECT(chk.cpu.pc() == Fixture::handler(6) && (chk.ccr() & kN) == 0);

    Fixture ok{0x4181};
    ok.cpu.set_d(0, 10);
    ok.cpu.set_d(1, 10);
    EXPECT(ok.cpu.step() == 10 && ok.cpu.pc() == kProgram + 2);

    Fixture trapv{0x4E76};
    trapv.set_ccr(kV);
    trapv.cpu.step();
    EXPECT(trapv.cpu.pc() == Fixture::handler(7));

    Fixture line_a{0xA123};
    line_a.cpu.step();
    EXPECT(line_a.cpu.pc() == Fixture::handler(10));
    EXPECT(line_a.bus->read32(kStack - 4) == kProgram);  // the instruction itself
}

void test_status_register_instructions() {
    Fixture f{0x40C0, 0x44FC, 0x001F, 0x027C, 0xF8FF};  // MOVE SR,D0 ; MOVE #$1F,CCR ; ANDI #$F8FF,SR
    f.cpu.set_sr(0x2700);
    f.cpu.step();
    EXPECT((f.cpu.d(0) & 0xFFFF) == 0x2700);
    f.cpu.step();
    EXPECT(f.cpu.sr() == 0x271F);
    f.cpu.step();
    EXPECT(f.cpu.sr() == 0x201F);  // interrupt mask cleared

    Fixture user{0x46FC, 0x2700};  // MOVE #$2700,SR in user mode
    user.cpu.set_sr(0x0000);
    user.cpu.set_a(7, 0x6000);
    user.cpu.step();
    EXPECT(user.cpu.pc() == Fixture::handler(8));

    Fixture usp{0x4E60, 0x4E69};  // MOVE A0,USP ; MOVE USP,A1
    usp.cpu.set_a(0, 0x1234);
    usp.cpu.step();
    usp.cpu.step();
    EXPECT(usp.cpu.usp() == 0x1234 && usp.cpu.a(1) == 0x1234);

    Fixture rtr{0x4E77};  // RTR: CCR then PC
    rtr.bus->write16(kStack, 0xFF1F);
    rtr.bus->write32(kStack + 2, 0x2000);
    rtr.cpu.set_sr(0x2700);
    rtr.cpu.step();
    EXPECT(rtr.cpu.sr() == 0x271F && rtr.cpu.pc() == 0x2000);
}

void test_stop_waits_for_interrupt() {
    Fixture f{0x4E72, 0x2000};  // STOP #$2000
    EXPECT(f.cpu.step() == 4);
    EXPECT(f.cpu.stopped() && f.cpu.sr() == 0x2000);
    EXPECT(f.cpu.step() == 4 && f.cpu.pc() == kProgram + 4);  // idle
    f.cpu.set_interrupt_level(3);
    EXPECT(f.cpu.step() == 44);
    EXPECT(!f.cpu.stopped() && f.cpu.pc() == Fixture::handler(27));
    EXPECT(f.bus->read32(kStack - 4) == kProgram + 4);  // returns after the STOP
}

int g_resets = 0;

void test_reset_instruction() {
    Fixture f{0x4E70};
    g_resets = 0;
    f.cpu.set_reset_handler([](void*) { ++g_resets; }, nullptr);
    EXPECT(f.cpu.step() == 132);
    EXPECT(g_resets == 1);
    EXPECT(f.cpu.pc() == kProgram + 2);
}

}  // namespace

void run_cpu_isa_tests() {
    test_moveq_lea_pea();
    test_movem();
    test_exg_swap_ext_link_unlk();
    test_movep();
    test_sub_family();
    test_neg_clr_not();
    test_mul_div();
    test_bcd();
    test_logic();
    test_shifts();
    test_dbcc_and_scc();
    test_traps();
    test_status_register_instructions();
    test_stop_waits_for_interrupt();
    test_reset_instruction();
}
