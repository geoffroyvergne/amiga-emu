#include <array>
#include <cstdint>
#include <memory>

#include "core/agnus.hpp"
#include "core/custom_registers.hpp"
#include "core/denise.hpp"
#include "core/memory_bus.hpp"
#include "test_framework.hpp"

namespace {

using amiga::Agnus;
using amiga::Denise;
using amiga::LineFetch;
using amiga::MemoryBus;
namespace reg = amiga::reg;

constexpr uint32_t kBlack = 0xFF00'0000;
constexpr uint32_t kWhite = 0xFFFF'FFFF;
constexpr uint16_t kTopLine = 0x2C;
constexpr uint32_t kPlaneBase = 0x1'0000;
constexpr uint32_t kPlaneStride = 0x2000;

uint16_t bplcon0(unsigned planes, uint16_t flags = 0) {
    return static_cast<uint16_t>(planes << reg::kBplCon0BpuShift | reg::kBplCon0Color | flags);
}

// Agnus + Denise with the standard PAL lowres window ($2C81-$2CC1, fetch
// $38-$D0), bitplane DMA on, and planes at kPlaneBase + n * kPlaneStride.
struct Fixture {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    Agnus agnus;
    Denise denise;
    LineFetch fetch;
    std::array<uint32_t, Denise::kOutputWidth> row{};
    amiga::Sprites sprites;

    Fixture() {
        write(reg::kDmaCon, reg::kSetClr | reg::kDmaEn | reg::kBplEn);
        write(reg::kDiwStrt, 0x2C81);
        write(reg::kDiwStop, 0x2CC1);
        write(reg::kDdfStrt, 0x0038);
        write(reg::kDdfStop, 0x00D0);
        write(reg::kColor00, 0x0000);
        write(reg::kColor00 + 2, 0x0FFF);
        for (unsigned plane = 0; plane < 6; ++plane) point_plane(plane, kPlaneBase + plane * kPlaneStride);
    }

    // Like the chipset: BPLCON0 goes to both chips.
    void write(uint16_t offset, uint16_t value) {
        agnus.write_register(offset, value);
        denise.write_register(offset, value);
    }

    void point_plane(unsigned plane, uint32_t address) {
        write(static_cast<uint16_t>(reg::kBpl1Pth + plane * 4), static_cast<uint16_t>(address >> 16));
        write(static_cast<uint16_t>(reg::kBpl1Ptl + plane * 4), static_cast<uint16_t>(address));
    }

    void poke16(uint32_t address, uint16_t value) {
        bus->chip_ram()[address] = static_cast<uint8_t>(value >> 8);
        bus->chip_ram()[address + 1] = static_cast<uint8_t>(value);
    }

    // Sets pixel x (0-15, first word of the line) to a colour index across planes.
    void set_pixel(unsigned x, unsigned index) {
        for (unsigned plane = 0; plane < 6; ++plane) {
            if ((index & (1u << plane)) == 0) continue;
            const uint32_t address = kPlaneBase + plane * kPlaneStride;
            const auto word = static_cast<uint16_t>(bus->chip_ram()[address] << 8 | bus->chip_ram()[address + 1]);
            poke16(address, static_cast<uint16_t>(word | (0x8000u >> x)));
        }
    }

    void render(uint16_t vpos = kTopLine) {
        agnus.fetch_line(bus->chip_ram(), vpos, fetch);
        denise.render_line(fetch, agnus.window(), sprites, row);
    }

    // Colour of lowres pixel x (both of its columns must agree).
    uint32_t lowres(unsigned x) const {
        return row[2 * x] == row[2 * x + 1] ? row[2 * x] : 0xDEAD'BEEF;
    }
};

// --- Registers ------------------------------------------------------------------

void test_palette_conversion() {
    EXPECT(Denise::rgb12_to_argb(0x0000) == 0xFF00'0000);
    EXPECT(Denise::rgb12_to_argb(0x0FFF) == 0xFFFF'FFFF);
    EXPECT(Denise::rgb12_to_argb(0x0F80) == 0xFFFF'8800);
    EXPECT(Denise::rgb12_to_argb(0x005A) == 0xFF00'55AA);
    EXPECT(Denise::half_brite(0x0FFF) == 0x0777);
    EXPECT(Denise::half_brite(0x0F81) == 0x0740);

    Denise d;
    EXPECT(d.palette(7) == kBlack);  // power-on: all black, opaque
    EXPECT(d.write_register(reg::kColor31, 0xF123));  // upper nibble unused
    EXPECT(d.color(31) == 0x0123);
    EXPECT(d.palette(31) == 0xFF11'2233);
    EXPECT(!d.write_register(reg::kBpl1Pth, 0));  // Agnus register, not Denise
}

void test_display_window_decoding() {
    Agnus a;
    a.write_register(reg::kDiwStrt, 0x2C81);
    a.write_register(reg::kDiwStop, 0x2CC1);
    auto w = a.window();
    EXPECT(w.hstart == 0x81 && w.hstop == 0x1C1);  // H8 = 1 for stop: 320 pixels
    EXPECT(w.vstart == 0x2C && w.vstop == 0x12C);  // V7 = 0 -> V8 = 1: 256 lines

    a.write_register(reg::kDiwStop, 0xF4C1);  // V7 = 1 -> V8 = 0
    w = a.window();
    EXPECT(w.vstop == 0xF4);
}

void test_fetch_geometry() {
    Agnus a;
    a.write_register(reg::kDdfStrt, 0x0038);
    a.write_register(reg::kDdfStop, 0x00D0);
    EXPECT(a.fetch_words() == 20);  // 320 lowres pixels
    EXPECT(a.first_pixel() == 0x81);

    a.write_register(reg::kBplCon0, bplcon0(2, reg::kBplCon0Hires));
    a.write_register(reg::kDdfStrt, 0x003C);
    a.write_register(reg::kDdfStop, 0x00D4);
    EXPECT(a.fetch_words() == 40);  // 640 hires pixels
    EXPECT(a.first_pixel() == 0x81);

    a.write_register(reg::kDdfStop, 0x00D0);  // Workbench 1.3's hires setting
    EXPECT(a.fetch_words() == 40);            // 8-cycle fetch units: still 40 words

    a.write_register(reg::kDdfStrt, 0x0010);  // below the $18 hardware limit
    EXPECT(a.first_pixel() == 2 * 0x18 + 9);
    a.write_register(reg::kDdfStop, 0x0010);  // stop before start: no fetch
    EXPECT(a.fetch_words() == 0);
}

void test_plane_count_limits() {
    Agnus a;
    a.write_register(reg::kBplCon0, bplcon0(6));
    EXPECT(a.fetched_planes() == 6);
    a.write_register(reg::kBplCon0, bplcon0(5, reg::kBplCon0Hires));
    EXPECT(a.fetched_planes() == 4);  // OCS has DMA slots for 4 hires planes
    a.write_register(reg::kBplCon0, bplcon0(7));
    EXPECT(a.fetched_planes() == 0);  // invalid
}

void test_bitplane_pointer_registers() {
    Agnus a;
    EXPECT(a.write_register(reg::kBpl1Pth + 8, 0xFFFF));  // BPL3PTH: only A18-A16 kept
    EXPECT(a.write_register(reg::kBpl1Ptl + 8, 0x1235));  // BPL3PTL: A0 not driven
    EXPECT(a.bplpt(2) == 0x7'1234);
    EXPECT(a.bplpt(0) == 0);
    EXPECT(!a.write_register(reg::kBplCon1, 0));  // Denise register, not Agnus
}

// --- Line rendering -------------------------------------------------------------

void test_lowres_single_plane() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.poke16(kPlaneBase, 0x8001);       // pixels 0 and 15
    f.poke16(kPlaneBase + 38, 0x4000);  // last word: pixel 305
    f.render();
    EXPECT(f.lowres(0) == kWhite);
    EXPECT(f.lowres(1) == kBlack);
    EXPECT(f.lowres(15) == kWhite);
    EXPECT(f.lowres(304) == kBlack);
    EXPECT(f.lowres(305) == kWhite);
    EXPECT(f.agnus.bplpt(0) == kPlaneBase + 40);
}

void test_hires_single_plane() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1, reg::kBplCon0Hires));
    f.write(reg::kDdfStrt, 0x003C);
    f.write(reg::kDdfStop, 0x00D4);
    f.poke16(kPlaneBase, 0xA000);
    f.poke16(kPlaneBase + 78, 0x0001);
    f.render();
    EXPECT(f.row[0] == kWhite && f.row[1] == kBlack && f.row[2] == kWhite);
    EXPECT(f.row[639] == kWhite);
    EXPECT(f.agnus.bplpt(0) == kPlaneBase + 80);
}

void test_five_planes_use_32_colours() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(5));
    for (uint16_t i = 0; i < 32; ++i) f.write(static_cast<uint16_t>(reg::kColor00 + 2 * i), static_cast<uint16_t>(i * 0x111 & 0xFFF));
    f.set_pixel(0, 31);
    f.set_pixel(1, 17);
    f.set_pixel(2, 6);
    f.render();
    EXPECT(f.lowres(0) == f.denise.palette(31));
    EXPECT(f.lowres(1) == f.denise.palette(17));
    EXPECT(f.lowres(2) == f.denise.palette(6));
    EXPECT(f.lowres(3) == f.denise.palette(0));
    for (unsigned plane = 0; plane < 5; ++plane) {
        EXPECT(f.agnus.bplpt(plane) == kPlaneBase + plane * kPlaneStride + 40);
    }
    EXPECT(f.agnus.bplpt(5) == kPlaneBase + 5 * kPlaneStride);  // not fetched
}

void test_extra_half_brite() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(6));
    f.write(reg::kColor00 + 2 * 5, 0x0EC8);
    f.set_pixel(0, 5);         // COLOR05
    f.set_pixel(1, 32 + 5);    // COLOR05 at half brightness
    f.set_pixel(2, 32);        // COLOR00 at half brightness
    f.render();
    EXPECT(f.lowres(0) == Denise::rgb12_to_argb(0x0EC8));
    EXPECT(f.lowres(1) == Denise::rgb12_to_argb(0x0764));
    EXPECT(f.lowres(2) == kBlack);
}

void test_dual_playfield() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(4, reg::kBplCon0Dblpf));
    for (uint16_t i = 0; i < 16; ++i) f.write(static_cast<uint16_t>(reg::kColor00 + 2 * i), static_cast<uint16_t>(0x100 + i));
    f.set_pixel(0, 0b0011);  // PF1 = 1 (plane 1), PF2 = 1 (plane 2)
    f.set_pixel(1, 0b0010);  // PF2 only
    f.set_pixel(3, 0b0100);  // PF1 = 2 (plane 3)
    f.set_pixel(4, 0b1000);  // PF2 = 2 (plane 4)

    f.render();
    EXPECT(f.lowres(0) == f.denise.palette(1));   // PF1 in front by default
    EXPECT(f.lowres(1) == f.denise.palette(9));   // PF1 transparent: PF2 shows (COLOR08 + 1)
    EXPECT(f.lowres(2) == f.denise.palette(0));   // both transparent
    EXPECT(f.lowres(3) == f.denise.palette(2));
    EXPECT(f.lowres(4) == f.denise.palette(10));

    for (unsigned plane = 0; plane < 4; ++plane) f.point_plane(plane, kPlaneBase + plane * kPlaneStride);
    f.write(reg::kBplCon2, reg::kBplCon2Pf2Pri);
    f.render();
    EXPECT(f.lowres(0) == f.denise.palette(9));   // PF2 in front
    EXPECT(f.lowres(3) == f.denise.palette(2));   // PF2 transparent there
}

void test_hold_and_modify() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(6, reg::kBplCon0Homod));
    f.write(reg::kColor00 + 2, 0x0123);
    f.set_pixel(0, 0b00'0001);  // palette: COLOR01
    f.set_pixel(1, 0b10'1111);  // modify red   -> $F23
    f.set_pixel(2, 0b11'0000);  // modify green -> $F03
    f.set_pixel(3, 0b01'1010);  // modify blue  -> $F0A
    f.render();
    EXPECT(f.lowres(0) == Denise::rgb12_to_argb(0x0123));
    EXPECT(f.lowres(1) == Denise::rgb12_to_argb(0x0F23));
    EXPECT(f.lowres(2) == Denise::rgb12_to_argb(0x0F03));
    EXPECT(f.lowres(3) == Denise::rgb12_to_argb(0x0F0A));
    EXPECT(f.lowres(4) == Denise::rgb12_to_argb(0x0000));  // index 0: palette COLOR00
}

void test_odd_and_even_modulos() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(2));
    f.write(reg::kBpl1Mod, 40);
    f.write(reg::kBpl2Mod, static_cast<uint16_t>(-40));
    f.render();
    EXPECT(f.agnus.bplpt(0) == kPlaneBase + 80);             // odd plane: skip a line
    EXPECT(f.agnus.bplpt(1) == kPlaneBase + kPlaneStride);  // even plane: repeat the line
}

void test_horizontal_window_frames_the_display() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    for (uint32_t a = 0; a < 40; a += 2) f.poke16(kPlaneBase + a, 0xFFFF);
    f.write(reg::kDiwStrt, 0x2C91);  // 16 pixels in from the left
    f.write(reg::kDiwStop, 0x2CB1);  // 16 pixels in from the right
    f.render();
    EXPECT(f.lowres(15) == kBlack);   // border (COLOR00) despite bitplane data
    EXPECT(f.lowres(16) == kWhite);
    EXPECT(f.lowres(303) == kWhite);
    EXPECT(f.lowres(304) == kBlack);
    EXPECT(f.agnus.bplpt(0) == kPlaneBase + 40);  // fetch unaffected by the window
}

void test_vertical_window_gates_dma() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.write(reg::kColor00, 0x0005);
    f.poke16(kPlaneBase, 0xFFFF);
    f.render(kTopLine - 1);  // above DIWSTRT
    EXPECT(!f.fetch.vertical_window);
    EXPECT(f.fetch.planes == 0);
    EXPECT(f.agnus.bplpt(0) == kPlaneBase);
    for (const uint32_t pixel : f.row) EXPECT(pixel == 0xFF00'0055);

    f.render(0x12C);  // DIWSTOP line: outside (exclusive)
    EXPECT(!f.fetch.vertical_window);
}

void test_scroll_delay_per_playfield() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.poke16(kPlaneBase, 0x8000);
    f.write(reg::kBplCon1, 0x0030);  // PF2H = 3: even planes only, plane 1 unaffected
    f.render();
    EXPECT(f.lowres(0) == kWhite);

    f.point_plane(0, kPlaneBase);
    f.write(reg::kBplCon1, 0x0003);  // PF1H = 3
    f.render();
    EXPECT(f.lowres(0) == kBlack);
    EXPECT(f.lowres(3) == kWhite);
}

void test_early_fetch_is_hidden_outside_window() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.write(reg::kDdfStrt, 0x0030);  // one fetch unit (16 pixels) earlier
    f.poke16(kPlaneBase, 0xFFFF);      // lands left of the window: hidden
    f.poke16(kPlaneBase + 2, 0x8000);  // first word inside the window
    f.render();
    EXPECT(f.agnus.fetch_words() == 21);
    EXPECT(f.lowres(0) == kWhite);
    EXPECT(f.lowres(1) == kBlack);
    EXPECT(f.agnus.bplpt(0) == kPlaneBase + 42);
}

void test_no_dma_shows_color00() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.write(reg::kDmaCon, reg::kBplEn);  // clear BPLEN
    f.write(reg::kColor00, 0x005A);
    f.poke16(kPlaneBase, 0xFFFF);
    f.render();
    for (const uint32_t pixel : f.row) EXPECT(pixel == 0xFF00'55AA);
    EXPECT(f.agnus.bplpt(0) == kPlaneBase);
}

// --- Sprites -------------------------------------------------------------------

void test_sprite_dma_sequence() {
    auto bus = std::make_unique<MemoryBus>();
    amiga::Sprites sprites;
    // POS: VSTART $30, HSTART $90 (H8-H1 = $48); CTL: VSTOP $32. Two lines of data, then 0/0.
    const uint16_t list[] = {0x3048, 0x3200, 0x8000, 0x0001, 0x4000, 0x0002, 0x0000, 0x0000};
    for (unsigned i = 0; i < std::size(list); ++i) {
        bus->chip_ram()[0x3000 + 2 * i] = static_cast<uint8_t>(list[i] >> 8);
        bus->chip_ram()[0x3000 + 2 * i + 1] = static_cast<uint8_t>(list[i]);
    }
    sprites.write_register(0x120, 0x0000);
    sprites.write_register(0x122, 0x3000);
    const auto& s = sprites.sprite(0);
    for (uint16_t v = 0; v < 0x30; ++v) sprites.dma_line(bus->chip_ram(), v, true);
    EXPECT(!s.armed && s.hstart() == 0x90 && s.vstart() == 0x30 && s.vstop() == 0x32);
    sprites.dma_line(bus->chip_ram(), 0x30, true);
    EXPECT(s.armed && s.data == 0x8000 && s.datb == 0x0001);
    sprites.dma_line(bus->chip_ram(), 0x31, true);
    EXPECT(s.armed && s.data == 0x4000 && s.datb == 0x0002);
    sprites.dma_line(bus->chip_ram(), 0x32, true);  // VSTOP: next control words (end marker)
    EXPECT(!s.armed);

    amiga::Sprites off;  // no SPREN: nothing fetched
    off.write_register(0x122, 0x3000);
    for (uint16_t v = 0; v < 0x31; ++v) off.dma_line(bus->chip_ram(), v, false);
    EXPECT(!off.sprite(0).armed);
}

void test_sprite_display_and_priority() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.write(static_cast<uint16_t>(reg::kColor00 + 2 * 17), 0x0F00);  // sprite 0/1 colour 1: red
    f.write(static_cast<uint16_t>(reg::kColor00 + 2 * 18), 0x00F0);  // colour 2: green
    // Sprite 0 at HSTART $80: its first pixel lands on lowres $81, the window's first pixel.
    f.sprites.write_register(0x140, 0x2C40);
    f.sprites.write_register(0x142, 0x2D00);
    f.sprites.write_register(0x146, 0x4000);  // DATB
    f.sprites.write_register(0x144, 0xC000);  // DATA (arms): pixel 0 = colour 1, pixel 1 = colour 3, pixel 2+ transparent
    f.poke16(kPlaneBase, 0x8000);             // playfield pixel 0 set (COLOR01 white)

    f.write(reg::kBplCon2, 0x0000);  // playfield in front of all sprites
    f.render();
    EXPECT(f.lowres(0) == kWhite);                  // playfield wins
    EXPECT(f.lowres(1) == f.denise.palette(19));    // playfield transparent there: sprite shows
    EXPECT(f.lowres(2) == kBlack);

    f.point_plane(0, kPlaneBase);
    f.write(reg::kBplCon2, 0x0008);  // PF2P = 1: sprite pair 0 in front
    f.render();
    EXPECT(f.lowres(0) == f.denise.palette(17));

    f.sprites.write_register(0x142, 0x2D00);  // writing CTL disarms
    f.point_plane(0, kPlaneBase);
    f.render();
    EXPECT(f.lowres(0) == kWhite && f.lowres(1) == kBlack);
}

void test_attached_sprites() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(0));
    for (uint16_t i = 16; i < 32; ++i) f.write(static_cast<uint16_t>(reg::kColor00 + 2 * i), static_cast<uint16_t>(0x100 + i));
    f.sprites.write_register(0x140, 0x2C40);  // sprite 0
    f.sprites.write_register(0x142, 0x2D00);
    f.sprites.write_register(0x146, 0x0000);
    f.sprites.write_register(0x144, 0x8000);  // bit 0 of the colour
    f.sprites.write_register(0x148, 0x2C40);  // sprite 1, attached
    f.sprites.write_register(0x14A, 0x2D80);
    f.sprites.write_register(0x14E, 0x8000);  // bit 3
    f.sprites.write_register(0x14C, 0x0000);  // bit 2 (arms)
    f.render();
    EXPECT(f.lowres(0) == f.denise.palette(16 + 0b1001));  // 4-bit colour from the pair
}

// Sprite 0 at HSTART h (lowres), 1 line tall, written at color clock `hpos`.
void place_sprite(amiga::Sprites& s, unsigned n, uint16_t hstart, uint16_t data, uint16_t datb = 0, uint16_t hpos = 0) {
    const auto base = static_cast<uint16_t>(0x140 + 8 * n);
    s.write_register(base, static_cast<uint16_t>(0x2C00 | (hstart >> 1)), hpos);
    s.write_register(static_cast<uint16_t>(base + 2), static_cast<uint16_t>(0x2D00 | (hstart & 1u)), hpos);
    s.write_register(static_cast<uint16_t>(base + 6), datb, hpos);
    s.write_register(static_cast<uint16_t>(base + 4), data, hpos);  // arms
}

void test_sprite_mid_line_multiplexing() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(0));
    f.write(static_cast<uint16_t>(reg::kColor00 + 2 * 17), 0x0F00);
    const uint32_t red = f.denise.palette(17);
    place_sprite(f.sprites, 0, 0x80, 0x8000);  // before the line: one pixel at lowres 0
    f.sprites.begin_line();
    // After the first trigger (position $80): new data, then move to $120 (lowres 160).
    f.sprites.write_register(0x144, 0xFFFF, 0x42);                        // DATA at position $84
    f.sprites.write_register(0x140, static_cast<uint16_t>(0x2C00 | (0x120 >> 1)), 0x60);  // POS at position $C0
    f.render();
    EXPECT(f.lowres(0) == red && f.lowres(1) == kBlack);  // first start: old data, 1 pixel
    for (unsigned x = 160; x < 176; ++x) EXPECT(f.lowres(x) == red);  // second start: new data, 16 pixels
    EXPECT(f.lowres(159) == kBlack && f.lowres(176) == kBlack);

    // A move to a position the beam has already passed: no second start.
    f.sprites.begin_line();
    f.sprites.write_register(0x140, static_cast<uint16_t>(0x2C00 | (0x90 >> 1)), 0x60);
    f.render();
    for (unsigned x = 0; x < 320; ++x) {
        if (f.lowres(x) != kBlack) EXPECT(x < 16);  // only the start at $80 of this line
    }
}

void test_sprite_collisions() {
    Fixture f;
    f.write(reg::kBplCon0, bplcon0(1));
    f.poke16(kPlaneBase, 0x8000);  // plane 1 set at lowres pixel 0 only
    (void)f.denise.read_collisions();

    // Plane 1 enabled, must be 1: sprite 0 over pixel 0 collides with the odd planes.
    f.write(Denise::kClxCon, 0x0041);  // ENBP1 | MVBP1
    place_sprite(f.sprites, 0, 0x80, 0x8000);
    f.render();
    uint16_t clx = f.denise.read_collisions();
    EXPECT((clx & 0x8000u) != 0);      // unused bit reads 1
    EXPECT((clx & (1u << 1)) != 0);    // odd planes vs sprites 0/1
    EXPECT((clx & (1u << 5)) != 0);    // even planes (none enabled: always match)
    EXPECT((f.denise.read_collisions() & 0x7FFFu) == 0);  // cleared by reading

    // Sprite 0 over a pixel where plane 1 is clear: no odd-plane collision.
    f.point_plane(0, kPlaneBase);
    place_sprite(f.sprites, 0, 0x81, 0x8000);  // lowres pixel 1
    f.render();
    clx = f.denise.read_collisions();
    EXPECT((clx & (1u << 1)) == 0);

    // Sprite groups: 0 and 2 overlap -> bit 9 (0/1 vs 2/3).
    f.point_plane(0, kPlaneBase);
    place_sprite(f.sprites, 2, 0x81, 0x8000);
    f.render();
    EXPECT((f.denise.read_collisions() & (1u << 9)) != 0);

    // Odd sprite 1 only counts with ENSP1.
    Fixture g;
    g.write(reg::kBplCon0, bplcon0(0));
    place_sprite(g.sprites, 1, 0x90, 0x8000);
    place_sprite(g.sprites, 4, 0x90, 0x8000);
    g.render();
    EXPECT((g.denise.read_collisions() & (1u << 10)) == 0);  // 0/1 vs 4/5: sprite 1 not enabled
    g.write(Denise::kClxCon, 0x1000);                        // ENSP1
    g.render();
    EXPECT((g.denise.read_collisions() & (1u << 10)) != 0);
}

}  // namespace

void run_display_tests() {
    test_palette_conversion();
    test_display_window_decoding();
    test_fetch_geometry();
    test_plane_count_limits();
    test_bitplane_pointer_registers();
    test_lowres_single_plane();
    test_hires_single_plane();
    test_five_planes_use_32_colours();
    test_extra_half_brite();
    test_dual_playfield();
    test_hold_and_modify();
    test_odd_and_even_modulos();
    test_horizontal_window_frames_the_display();
    test_vertical_window_gates_dma();
    test_scroll_delay_per_playfield();
    test_early_fetch_is_hidden_outside_window();
    test_no_dma_shows_color00();
    test_sprite_dma_sequence();
    test_sprite_display_and_priority();
    test_attached_sprites();
    test_sprite_mid_line_multiplexing();
    test_sprite_collisions();
}
