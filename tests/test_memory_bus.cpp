#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "core/memory_bus.hpp"
#include "test_framework.hpp"

namespace {

using amiga::BusError;
using amiga::MemoryBus;

// Fake 256KB Kickstart: $1111 header, reset PC at offset 4, then a byte ramp.
std::vector<uint8_t> make_fake_rom() {
    std::vector<uint8_t> rom(MemoryBus::kRomSize);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<uint8_t>(i * 7);
    const uint8_t header[] = {0x11, 0x11, 0x4E, 0xF9, 0x00, 0xFC, 0x00, 0xD2};
    std::copy(std::begin(header), std::end(header), rom.begin());
    return rom;
}

std::unique_ptr<MemoryBus> make_bus_without_overlay() {
    auto bus = std::make_unique<MemoryBus>();
    bus->set_overlay(false);
    return bus;
}

void test_big_endian_word_and_long() {
    auto bus = make_bus_without_overlay();
    bus->write32(0x1000, 0x1234'5678);
    EXPECT(bus->read8(0x1000) == 0x12);
    EXPECT(bus->read8(0x1001) == 0x34);
    EXPECT(bus->read8(0x1002) == 0x56);
    EXPECT(bus->read8(0x1003) == 0x78);
    EXPECT(bus->read16(0x1000) == 0x1234);
    EXPECT(bus->read16(0x1002) == 0x5678);
    EXPECT(bus->read32(0x1000) == 0x1234'5678);

    bus->write16(0x2000, 0xBEEF);
    EXPECT(bus->chip_ram()[0x2000] == 0xBE);
    EXPECT(bus->chip_ram()[0x2001] == 0xEF);

    bus->write8(0x3001, 0xAB);
    EXPECT(bus->read16(0x3000) == 0x00AB);
}

void test_chip_ram_bounds_and_mirroring() {
    auto bus = make_bus_without_overlay();
    bus->write32(MemoryBus::kChipRamSize - 4, 0xCAFE'F00D);
    EXPECT(bus->read32(MemoryBus::kChipRamSize - 4) == 0xCAFE'F00D);

    // 512KB repeats through $1FFFFF.
    bus->write8(0x000010, 0x5A);
    EXPECT(bus->read8(0x080010) == 0x5A);
    EXPECT(bus->read8(0x100010) == 0x5A);
    EXPECT(bus->read8(0x180010) == 0x5A);
    bus->write8(0x180020, 0xA5);
    EXPECT(bus->read8(0x000020) == 0xA5);

    // A long read at the end of chip RAM continues into its first mirror.
    bus->write16(0x000000, 0x4242);
    EXPECT(bus->read32(MemoryBus::kChipRamSize - 2) == 0xF00D'4242u);
}

void test_slow_ram() {
    auto bus = make_bus_without_overlay();
    bus->write32(0xC00000, 0x1234'5678);
    bus->write16(0xC7FFFE, 0xABCD);
    EXPECT(bus->read32(0xC00000) == 0x1234'5678);
    EXPECT(bus->read16(0xC7FFFE) == 0xABCD);
    EXPECT(bus->read16(0x000000) == 0);  // separate from chip RAM
}

void test_24bit_address_wrap() {
    auto bus = make_bus_without_overlay();
    bus->write16(0x0000'4000, 0x1357);
    EXPECT(bus->read16(0xFF00'4000) == 0x1357);  // A24-A31 ignored
}

void test_unmapped_access_panics() {
    auto bus = make_bus_without_overlay();
    EXPECT_THROWS(bus->read8(0x200000), BusError);    // just above the chip window
    EXPECT_THROWS(bus->read16(0xDFF006), BusError);   // custom chips: not emulated yet
    EXPECT_THROWS(bus->write8(0xBFE001, 0), BusError);  // CIA-A: not emulated yet
    EXPECT_THROWS(bus->read32(0xFC0000), BusError);   // no ROM loaded

    try {
        bus->write16(0xC80000, 0);  // just past the slow RAM
        EXPECT(false);
    } catch (const BusError& e) {
        EXPECT(e.address() == 0xC80000);
        EXPECT(e.size_bytes() == 2);
        EXPECT(e.is_write());
    }
}

void test_kickstart_mapping() {
    auto bus = std::make_unique<MemoryBus>();
    const auto rom = make_fake_rom();
    bus->load_kickstart(rom);
    EXPECT(bus->kickstart_loaded());

    EXPECT(bus->read16(0xFC0000) == 0x1111);
    EXPECT(bus->read32(0xFC0004) == 0x00FC'00D2);
    EXPECT(bus->read8(0xFFFFFF) == rom.back());
    EXPECT(bus->read32(0xF80000) == bus->read32(0xFC0000));  // 256KB repeat at $F80000

    // ROM is read-only: writes are ignored.
    bus->write16(0xFC0000, 0x0000);
    EXPECT(bus->read16(0xFC0000) == 0x1111);
}

void test_overlay_exposes_reset_vectors() {
    auto bus = std::make_unique<MemoryBus>();
    bus->load_kickstart(make_fake_rom());
    EXPECT(bus->overlay());  // on at power-on

    // The 68000 reads SSP from $000000 and PC from $000004 at reset.
    EXPECT(bus->read32(0x000000) == 0x1111'4EF9);
    EXPECT(bus->read32(0x000004) == 0x00FC'00D2);

    bus->set_overlay(false);
    EXPECT(bus->read32(0x000004) == 0);  // chip RAM visible again
    bus->write32(0x000004, 0xDEAD'BEEF);

    bus->reset();
    EXPECT(bus->read32(0x000004) == 0x00FC'00D2);
    bus->set_overlay(false);
    EXPECT(bus->read32(0x000004) == 0xDEAD'BEEF);  // RAM survives reset
}

void test_kickstart_rejects_bad_images() {
    auto bus = std::make_unique<MemoryBus>();
    const std::vector<uint8_t> too_small(128 * 1024);
    const std::vector<uint8_t> too_big(512 * 1024);
    EXPECT_THROWS(bus->load_kickstart(too_small), std::runtime_error);
    EXPECT_THROWS(bus->load_kickstart(too_big), std::runtime_error);

    std::vector<uint8_t> cloanto(MemoryBus::kRomSize + 11);
    const char magic[] = "AMIROMTYPE1";
    std::copy(magic, magic + 11, cloanto.begin());
    EXPECT_THROWS(bus->load_kickstart(cloanto), std::runtime_error);

    EXPECT(!bus->kickstart_loaded());
    EXPECT_THROWS(bus->load_kickstart(std::filesystem::path("/nonexistent/kick.rom")),
                  std::runtime_error);
}

void test_kickstart_load_from_file() {
    const auto rom = make_fake_rom();
    const auto path = std::filesystem::temp_directory_path() / "amiga_emu_test_kick.rom";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }
    auto bus = std::make_unique<MemoryBus>();
    bus->load_kickstart(path);
    std::filesystem::remove(path);

    EXPECT(bus->read32(0xFC0004) == 0x00FC'00D2);
    EXPECT(bus->read8(0xFC0000 + 1000) == rom[1000]);
}

}  // namespace

void run_memory_bus_tests() {
    test_big_endian_word_and_long();
    test_chip_ram_bounds_and_mirroring();
    test_24bit_address_wrap();
    test_slow_ram();
    test_unmapped_access_panics();
    test_kickstart_mapping();
    test_overlay_exposes_reset_vectors();
    test_kickstart_rejects_bad_images();
    test_kickstart_load_from_file();
}
