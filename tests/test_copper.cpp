#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "core/agnus.hpp"
#include "core/copper.hpp"
#include "core/custom_registers.hpp"
#include "core/memory_bus.hpp"
#include "core/timing.hpp"
#include "test_framework.hpp"

namespace {

using amiga::Copper;
using amiga::MemoryBus;
namespace reg = amiga::reg;

constexpr uint32_t kList1 = 0x1000;
constexpr uint32_t kList2 = 0x2000;
constexpr uint16_t kEnd[] = {0xFFFF, 0xFFFE};

struct Write {
    uint16_t offset;
    uint16_t value;
    uint16_t vpos;
    uint16_t hpos;
};

// Records register writes with the beam position; Copper registers are
// routed back to the Copper, as the chipset does.
struct RecordingPort final : amiga::CustomChipPort {
    Copper* copper = nullptr;
    std::vector<Write> writes;
    uint16_t vpos = 0;
    uint16_t hpos = 0;

    uint16_t read_custom(uint16_t) override { return 0; }
    void write_custom(uint16_t offset, uint16_t value) override {
        writes.push_back({offset, value, vpos, hpos});
        copper->write_register(offset, value);
    }
};

struct Fixture {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    Copper copper;
    RecordingPort port;

    Fixture() {
        port.copper = &copper;
        copper.write_register(reg::kCop1Lch, 0);
        copper.write_register(reg::kCop1Lcl, kList1);
        copper.write_register(reg::kCop2Lch, 0);
        copper.write_register(reg::kCop2Lcl, kList2);
    }

    void load(uint32_t address, std::initializer_list<uint16_t> words) {
        for (const uint16_t w : words) {
            bus->chip_ram()[address] = static_cast<uint8_t>(w >> 8);
            bus->chip_ram()[address + 1] = static_cast<uint8_t>(w);
            address += 2;
        }
    }

    void tick(uint16_t vpos, uint16_t hpos, bool dma = true) {
        port.vpos = vpos;
        port.hpos = hpos;
        copper.tick(bus->chip_ram(), vpos, hpos, dma, port);
    }

    // Runs the Copper over a whole frame, restarting at vertical blank.
    void run_frame() {
        copper.vertical_blank();
        for (uint16_t v = 0; v < amiga::timing::kPalLinesPerFrame; ++v) {
            for (uint16_t h = 0; h < amiga::timing::kPalColorClocksPerLine; ++h) tick(v, h);
        }
    }
};

void test_wait_comparator() {
    // WAIT $2C07,$FFFE: line $2C, horizontal $06 (H0 is never compared).
    EXPECT(!Copper::beam_reached(0x2C07, 0xFFFE, 0x2B, 0xE2));
    EXPECT(!Copper::beam_reached(0x2C07, 0xFFFE, 0x2C, 0x04));
    EXPECT(Copper::beam_reached(0x2C07, 0xFFFE, 0x2C, 0x06));
    EXPECT(Copper::beam_reached(0x2C07, 0xFFFE, 0x2D, 0x00));

    // V8 is not compared: line 300 ($12C) looks like line $2C.
    EXPECT(Copper::beam_reached(0x2C07, 0xFFFE, 0x12C, 0x06));
    EXPECT(!Copper::beam_reached(0xFF07, 0xFFFE, 0x12C, 0x06));

    // VE = 0: only V7 is compared, so this waits for hpos $40 on each of lines $00-$7F.
    EXPECT(!Copper::beam_reached(0x0041, 0x80FE, 0x50, 0x3E));
    EXPECT(Copper::beam_reached(0x0041, 0x80FE, 0x50, 0x40));
    EXPECT(Copper::beam_reached(0x0041, 0x80FE, 0x90, 0x00));  // V7 set: past the position

    // End-of-list WAIT $FFFF,$FFFE is never satisfied.
    bool reached = false;
    for (uint16_t v = 0; v < amiga::timing::kPalLinesPerFrame; ++v) {
        for (uint16_t h = 0; h < amiga::timing::kPalColorClocksPerLine; ++h) {
            reached = reached || Copper::beam_reached(0xFFFF, 0xFFFE, v, h);
        }
    }
    EXPECT(!reached);
}

void test_move_uses_two_even_cycles() {
    Fixture f;
    f.load(kList1, {reg::kColor00, 0x0F00, kEnd[0], kEnd[1]});
    f.copper.vertical_blank();

    f.tick(0, 0);  // fetch IR1
    f.tick(0, 1);  // odd cycle: not a Copper slot
    EXPECT(f.port.writes.empty());
    f.tick(0, 2);  // fetch IR2 and write
    EXPECT(f.port.writes.size() == 1);
    EXPECT(f.port.writes[0].offset == reg::kColor00);
    EXPECT(f.port.writes[0].value == 0x0F00);
    EXPECT(f.copper.pc() == kList1 + 4);
}

void test_dma_disabled_stalls() {
    Fixture f;
    f.load(kList1, {reg::kColor00, 0x0F00});
    f.copper.vertical_blank();
    for (uint16_t h = 0; h < 20; h += 2) f.tick(0, h, false);
    EXPECT(f.port.writes.empty());
    EXPECT(f.copper.pc() == kList1);
}

void test_wait_then_move() {
    Fixture f;
    f.load(kList1, {0x1007, 0xFFFE, reg::kColor00, 0x0123, kEnd[0], kEnd[1]});
    f.run_frame();

    EXPECT(f.port.writes.size() == 1);
    // Satisfied at $10/$06, wake-up at $08, IR1 at $0A, IR2 + write at $0C.
    EXPECT(f.port.writes[0].vpos == 0x10);
    EXPECT(f.port.writes[0].hpos == 0x0C);
    EXPECT(f.copper.state() == Copper::State::Waiting);  // parked on the end-of-list WAIT
}

void test_copper_bars_one_write_per_line() {
    Fixture f;
    std::vector<uint16_t> list;
    for (uint16_t line = 0x40; line < 0x48; ++line) {
        f.load(kList1 + static_cast<uint32_t>(list.size()) * 2,
               {static_cast<uint16_t>(line << 8 | 0x07), 0xFFFE, reg::kColor00, line});
        list.resize(list.size() + 4);
    }
    f.load(kList1 + static_cast<uint32_t>(list.size()) * 2, {kEnd[0], kEnd[1]});
    f.run_frame();

    EXPECT(f.port.writes.size() == 8);
    for (size_t i = 0; i < f.port.writes.size(); ++i) {
        EXPECT(f.port.writes[i].vpos == 0x40 + i);
        EXPECT(f.port.writes[i].value == 0x40 + i);
    }
}

void test_skip() {
    Fixture taken;
    taken.load(kList1, {0x0001, 0xFFFF,  // SKIP $0000: always reached
                        reg::kColor00, 0x0001, reg::kColor00, 0x0002, kEnd[0], kEnd[1]});
    taken.run_frame();
    EXPECT(taken.port.writes.size() == 1);
    EXPECT(taken.port.writes[0].value == 0x0002);

    Fixture not_taken;
    not_taken.load(kList1, {0xFF01, 0xFFFF,  // SKIP $FF00: not reached at line 0
                            reg::kColor00, 0x0001, reg::kColor00, 0x0002, kEnd[0], kEnd[1]});
    not_taken.run_frame();
    EXPECT(not_taken.port.writes.size() == 2);
}

void test_protected_registers_stop_the_copper() {
    Fixture f;
    f.load(kList1, {0x0040, 0x09F0, reg::kColor00, 0x0001, kEnd[0], kEnd[1]});  // BLTCON0
    f.run_frame();
    EXPECT(f.port.writes.empty());
    EXPECT(f.copper.state() == Copper::State::Stopped);

    f.copper.write_register(reg::kCopCon, Copper::kCopConDanger);
    f.run_frame();  // restarted by vertical blank; CDANG allows $40-$7E
    EXPECT(f.port.writes.size() == 2);

    Fixture low;
    low.copper.write_register(reg::kCopCon, Copper::kCopConDanger);
    low.load(kList1, {0x0020, 0x0000, reg::kColor00, 0x0001});  // below $40: never allowed
    low.run_frame();
    EXPECT(low.port.writes.empty());
}

void test_copjmp2_from_copper() {
    Fixture f;
    f.load(kList1, {reg::kCopJmp2, 0x0000, reg::kColor00, 0x0BAD, kEnd[0], kEnd[1]});
    f.load(kList2, {reg::kColor00, 0x0ABC, kEnd[0], kEnd[1]});
    f.run_frame();

    EXPECT(f.port.writes.size() == 2);
    EXPECT(f.port.writes[1].offset == reg::kColor00);
    EXPECT(f.port.writes[1].value == 0x0ABC);
}

void test_location_registers() {
    Copper c;
    c.write_register(reg::kCop1Lch, 0xFFFF);
    c.write_register(reg::kCop1Lcl, 0x1235);
    EXPECT(c.cop1lc() == 0x7'1234);  // A18-A1
    EXPECT(c.state() == Copper::State::Stopped);
    c.write_register(reg::kCopJmp1, 0);
    EXPECT(c.pc() == 0x7'1234);
    EXPECT(c.state() == Copper::State::FetchIr1);
}

}  // namespace

void run_copper_tests() {
    test_wait_comparator();
    test_move_uses_two_even_cycles();
    test_dma_disabled_stalls();
    test_wait_then_move();
    test_copper_bars_one_write_per_line();
    test_skip();
    test_protected_registers_stop_the_copper();
    test_copjmp2_from_copper();
    test_location_registers();
}
