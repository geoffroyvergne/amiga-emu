#include <cstdint>
#include <initializer_list>
#include <memory>

#include "core/chipset.hpp"
#include "core/cpu68000.hpp"
#include "core/custom_registers.hpp"
#include "core/machine.hpp"
#include "core/memory_bus.hpp"
#include "core/paula.hpp"
#include "test_framework.hpp"

namespace {

using amiga::BusError;
using amiga::Chipset;
using amiga::Cpu68000;
using amiga::Machine;
using amiga::MemoryBus;
using amiga::Paula;
namespace reg = amiga::reg;

constexpr uint32_t custom(uint16_t offset) { return MemoryBus::kCustomBase + offset; }

void poke(MemoryBus& bus, uint32_t address, std::initializer_list<uint16_t> words) {
    for (const uint16_t w : words) {
        bus.write16(address, w);
        address += 2;
    }
}

// --- Paula ----------------------------------------------------------------------

void test_paula_levels() {
    Paula p;
    p.request(reg::kIntVertb);
    EXPECT(p.interrupt_level() == 0);  // not enabled

    p.write_register(reg::kIntEna, reg::kSetClr | reg::kIntVertb);
    EXPECT(p.interrupt_level() == 0);  // master INTEN still off

    p.write_register(reg::kIntEna, reg::kSetClr | reg::kIntEn | reg::kIntPorts | reg::kIntExter);
    EXPECT(p.interrupt_level() == 3);

    p.request(reg::kIntPorts);
    EXPECT(p.interrupt_level() == 3);  // highest pending wins
    p.request(reg::kIntExter);
    EXPECT(p.interrupt_level() == 6);

    p.write_register(reg::kIntReq, reg::kIntExter | reg::kIntVertb);  // clear
    EXPECT(p.interrupt_level() == 2);
    EXPECT(p.intreq() == reg::kIntPorts);
}

// --- CPU interrupt handling -----------------------------------------------------

struct CpuFixture {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    Cpu68000 cpu{*bus};

    CpuFixture() {
        bus->set_overlay(false);
        poke(*bus, 0x1000, {0x4EF9, 0x0000, 0x1000});  // JMP $1000
        poke(*bus, 0x006C, {0x0000, 0x2000});          // level 3 autovector -> $2000
        poke(*bus, 0x007C, {0x0000, 0x3000});          // level 7 autovector -> $3000
        poke(*bus, 0x2000, {0x4E73});                  // RTE
        cpu.set_a(7, 0x8000);  // SSP
        cpu.set_pc(0x1000);
    }
};

// Levels 1-6 through assert_interrupt: autovector at $60 + 4 * level, SR
// mask raised to the level, PC and SR stacked on the supervisor stack.
void test_autovectors_for_all_levels() {
    for (int level = 1; level <= 6; ++level) {
        auto bus = std::make_unique<MemoryBus>();
        bus->set_overlay(false);
        poke(*bus, 0x1000, {0x4E71});  // NOP
        const auto vector = static_cast<uint32_t>(0x60 + 4 * level);
        bus->write32(vector, 0x2000u + static_cast<uint32_t>(level) * 0x100u);
        bus->write16(0x2000u + static_cast<uint32_t>(level) * 0x100u, 0x4E71);  // handler: NOP

        Cpu68000 cpu{*bus};
        cpu.set_a(7, 0x8000);        // SSP
        cpu.set_sr(0x0000);          // user mode, mask 0
        cpu.set_a(7, 0x4000);        // USP
        cpu.set_pc(0x1000);
        cpu.assert_interrupt(level);

        EXPECT(cpu.step() == 44);
        EXPECT(cpu.pc() == 0x2000u + static_cast<uint32_t>(level) * 0x100u);
        EXPECT(cpu.supervisor());
        EXPECT(cpu.interrupt_mask() == level);
        EXPECT(cpu.a(7) == 0x8000 - 6);              // pushed on the supervisor stack...
        EXPECT(cpu.usp() == 0x4000);                 // ...not the user stack
        EXPECT(bus->read16(0x8000 - 6) == 0x0000);   // old SR
        EXPECT(bus->read32(0x8000 - 4) == 0x1000);   // return PC
        EXPECT(cpu.exception_count(static_cast<uint8_t>(24 + level)) == 1);

        // Still asserted, but now equal to the mask: not taken again.
        EXPECT(cpu.step() == 4);  // the handler's NOP runs
        EXPECT(cpu.exception_count(static_cast<uint8_t>(24 + level)) == 1);

        // At or below the mask: not taken.
        Cpu68000 masked{*bus};
        masked.set_a(7, 0x8000);
        masked.set_sr(static_cast<uint16_t>(0x2000 | level << 8));
        masked.set_pc(0x1000);
        masked.assert_interrupt(level);
        masked.step();
        EXPECT(masked.pc() == 0x1002);  // executed the NOP
    }
}

void test_interrupt_respects_mask() {
    CpuFixture f;
    f.cpu.set_interrupt_level(3);
    f.cpu.set_sr(0x2700);
    f.cpu.step();
    EXPECT(f.cpu.pc() == 0x1000);  // masked: executed the JMP

    f.cpu.set_sr(0x2300);
    f.cpu.step();
    EXPECT(f.cpu.pc() == 0x1000);  // level must be strictly above the mask
}

void test_interrupt_exception_frame() {
    CpuFixture f;
    f.cpu.set_sr(0x2000 | Cpu68000::kFlagT | Cpu68000::kFlagC);
    f.cpu.set_interrupt_level(3);

    EXPECT(f.cpu.step() == 44);
    EXPECT(f.cpu.pc() == 0x2000);
    EXPECT(f.cpu.sr() == (0x2300 | Cpu68000::kFlagC));  // T cleared, mask = 3, CCR kept
    EXPECT(f.cpu.a(7) == 0x8000 - 6);
    EXPECT(f.bus->read16(0x7FFA) == (0x2000 | Cpu68000::kFlagT | Cpu68000::kFlagC));  // old SR
    EXPECT(f.bus->read32(0x7FFC) == 0x1000);  // return PC
}

void test_rte_returns_to_user_mode() {
    CpuFixture f;
    f.cpu.set_sr(0x0000);  // user mode, mask 0
    f.cpu.set_a(7, 0x4000);  // USP
    f.cpu.set_interrupt_level(3);

    f.cpu.step();  // interrupt: switches to SSP ($8000)
    EXPECT(f.cpu.supervisor());
    EXPECT(f.cpu.a(7) == 0x8000 - 6);
    EXPECT(f.cpu.usp() == 0x4000);

    f.cpu.set_interrupt_level(0);  // handler acknowledged the request
    EXPECT(f.cpu.step() == 20);    // RTE
    EXPECT(f.cpu.pc() == 0x1000);
    EXPECT(f.cpu.sr() == 0x0000);
    EXPECT(f.cpu.a(7) == 0x4000);
    EXPECT(f.cpu.ssp() == 0x8000);
}

void test_rte_in_user_mode_is_a_privilege_violation() {
    CpuFixture f;
    poke(*f.bus, 0x0020, {0x0000, 0x5000});  // privilege violation vector -> $5000
    f.cpu.set_sr(0x0000);
    f.cpu.set_a(7, 0x4000);  // USP
    f.cpu.set_pc(0x2000);
    EXPECT(f.cpu.step() == 34);
    EXPECT(f.cpu.pc() == 0x5000);
    EXPECT(f.cpu.supervisor());
    EXPECT(f.bus->read32(0x8000 - 4) == 0x2000);  // on the supervisor stack
}

void test_nmi_is_edge_triggered() {
    CpuFixture f;
    f.cpu.set_sr(0x2700);
    f.cpu.set_interrupt_level(7);
    f.cpu.step();
    EXPECT(f.cpu.pc() == 0x3000);  // taken despite mask 7

    f.cpu.set_pc(0x1000);
    f.cpu.set_interrupt_level(7);  // still held: no new edge
    f.cpu.step();
    EXPECT(f.cpu.pc() == 0x1000);
}

// --- Chipset through the bus ----------------------------------------------------

void test_custom_registers_through_bus() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();

    bus.write16(custom(reg::kColor00), 0x0F00);
    EXPECT(m->chipset().denise().color(0) == 0x0F00);

    bus.write8(custom(reg::kColor00 + 3), 0x12);  // byte write: seen as $1212 by the chip
    EXPECT(m->chipset().denise().color(1) == 0x0212);

    bus.write16(custom(reg::kDmaCon), reg::kSetClr | reg::kDmaEn | reg::kCopEn | reg::kBplEn);
    bus.write16(custom(reg::kDmaCon), reg::kBplEn);  // clear
    EXPECT(bus.read16(custom(reg::kDmaConR)) == (reg::kDmaEn | reg::kCopEn));

    bus.write16(custom(reg::kIntEna), reg::kSetClr | reg::kIntEn | reg::kIntVertb);
    EXPECT(bus.read16(custom(reg::kIntEnaR)) == (reg::kIntEn | reg::kIntVertb));
    EXPECT(bus.read8(custom(reg::kIntEnaR)) == 0x40);  // high byte
    EXPECT(bus.read8(custom(reg::kIntEnaR) + 1) == 0x20);  // low byte

    EXPECT(bus.read16(custom(reg::kVPosR)) == 0x8000);  // PAL OCS Agnus, long frame, V8 = 0

    // Only A1-A8 reach the custom chips: the block repeats through $C80000-$DFFFFF.
    EXPECT(bus.read16(custom(0x200) + reg::kIntEnaR) == (reg::kIntEn | reg::kIntVertb));
    EXPECT(bus.read16(0xC8F000 + reg::kIntEnaR) == (reg::kIntEn | reg::kIntVertb));
    bus.write16(0xD00000 + reg::kColor00, 0x0ABC);
    EXPECT(m->chipset().denise().color(0) == 0x0ABC);
    EXPECT(bus.read16(0xC00000) == 0);  // slow RAM, not a mirror
    EXPECT_THROWS(bus.read16(0xE00000), BusError);
}

void test_beam_counter_and_vertb() {
    auto m = std::make_unique<Machine>();
    m->run_frame();  // CPU halted: only the chipset runs
    EXPECT(m->chipset().agnus().vpos() == 0);
    EXPECT(m->chipset().agnus().hpos() == 0);
    EXPECT((m->chipset().paula().intreq() & reg::kIntVertb) != 0);

    for (int i = 0; i < 0x2C * 227 + 0x40; ++i) m->chipset().tick();
    EXPECT(m->bus().read16(custom(reg::kVHPosR)) == 0x2C40);

    for (int i = 0; i < 300 * 227; ++i) m->chipset().tick();  // line $2C + 300 = 344 - 313 = 31
    EXPECT(m->chipset().agnus().vpos() == 31);
}

// Copper bars: COLOR00 changes on display line 10, bitplane shown on top.
void test_copper_drives_display() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();
    constexpr uint32_t kBitmap = 0x1'0000;
    constexpr uint32_t kList = 0x2'0000;
    constexpr uint16_t kLine10 = Chipset::kFirstDisplayLine + 10;

    bus.write16(kBitmap + 40 * 10, 0x8000);  // leftmost pixel of display line 10
    poke(bus, kList, {
        reg::kBpl1Pth, 0x0001, reg::kBpl1Ptl, 0x0000,
        reg::kColor00, 0x0005,
        static_cast<uint16_t>(kLine10 << 8 | 0x07), 0xFFFE,
        reg::kColor00, 0x0F00,
        0xFFFF, 0xFFFE,
    });
    bus.write16(custom(reg::kColor00 + 2), 0x0FFF);
    bus.write16(custom(reg::kBplCon0), 0x1200);
    bus.write16(custom(reg::kDiwStrt), 0x2C81);
    bus.write16(custom(reg::kDiwStop), 0x2CC1);
    bus.write16(custom(reg::kDdfStrt), 0x0038);
    bus.write16(custom(reg::kDdfStop), 0x00D0);
    EXPECT(m->chipset().agnus().fetched_planes() == 1);  // BPLCON0 reached Agnus...
    EXPECT(m->chipset().denise().bplcon0() == 0x1200);   // ...and Denise
    bus.write16(custom(reg::kCop1Lch), 0x0002);
    bus.write16(custom(reg::kCop1Lcl), 0x0000);
    bus.write16(custom(reg::kDmaCon), reg::kSetClr | reg::kDmaEn | reg::kBplEn | reg::kCopEn);

    m->run_frame();
    m->run_frame();  // second frame starts with COLOR00 back to $005, set by the Copper

    const auto& frame = m->chipset().frame();
    const auto pixel = [&frame](size_t line, size_t x) { return frame[line * 640 + x]; };
    EXPECT(pixel(0, 0) == 0xFF00'0055);
    EXPECT(pixel(9, 0) == 0xFF00'0055);
    EXPECT(pixel(10, 0) == 0xFFFF'FFFF);  // bitplane pixel (COLOR01)
    EXPECT(pixel(10, 2) == 0xFFFF'0000);  // background now red
    EXPECT(pixel(255, 639) == 0xFFFF'0000);
}

// Full path: VERTB (Paula) -> level 3 autovector (CPU) -> handler with RTE,
// with the Copper acknowledging the request by writing INTREQ.
void test_vertical_blank_interrupt_end_to_end() {
    auto m = std::make_unique<Machine>();
    MemoryBus& bus = m->bus();

    poke(bus, 0x0000, {0x0008, 0x0000, 0x0000, 0x1000});  // reset: SSP $80000, PC $1000
    poke(bus, 0x006C, {0x0000, 0x2000});                  // level 3 autovector
    poke(bus, 0x1000, {0x4EF9, 0x0000, 0x1000});          // main: JMP $1000
    poke(bus, 0x2000, {0xD081, 0x4E73});                  // handler: ADD.L D1,D0 ; RTE
    poke(bus, 0x3000, {0x0107, 0xFFFE,                // Copper: at line 1,
                       reg::kIntReq, reg::kIntVertb,  // clear VERTB
                       0xFFFF, 0xFFFE});

    bus.write16(custom(reg::kCop1Lch), 0x0000);
    bus.write16(custom(reg::kCop1Lcl), 0x3000);
    bus.write16(custom(reg::kIntEna), reg::kSetClr | reg::kIntEn | reg::kIntVertb);
    bus.write16(custom(reg::kDmaCon), reg::kSetClr | reg::kDmaEn | reg::kCopEn);

    m->reset();
    m->cpu().set_sr(0x2000);  // no MOVE to SR yet: lower the mask by hand
    m->cpu().set_d(1, 1);

    m->run_frame();
    const uint32_t first = m->cpu().d(0);
    EXPECT(first > 0);  // the handler ran until the Copper cleared VERTB
    EXPECT(!m->cpu_halted());
    EXPECT((m->chipset().paula().intreq() & reg::kIntVertb) == 0);
    EXPECT(m->cpu().pc() == 0x1000);
    EXPECT(m->cpu().interrupt_mask() == 0);
    EXPECT(m->cpu().a(7) == 0x8'0000);  // stack balanced

    m->run_frame();
    EXPECT(m->cpu().d(0) > first);
}

}  // namespace

void run_machine_tests() {
    test_paula_levels();
    test_autovectors_for_all_levels();
    test_interrupt_respects_mask();
    test_interrupt_exception_frame();
    test_rte_returns_to_user_mode();
    test_rte_in_user_mode_is_a_privilege_violation();
    test_nmi_is_edge_triggered();
    test_custom_registers_through_bus();
    test_beam_counter_and_vertb();
    test_copper_drives_display();
    test_vertical_blank_interrupt_end_to_end();
}
