#include "core/memory_bus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace amiga {

namespace {

const char* size_suffix(uint8_t size) noexcept {
    switch (size) {
        case 1: return "B";
        case 2: return "W";
        default: return "L";
    }
}

}  // namespace

BusError::BusError(uint32_t address, uint8_t size_bytes, bool is_write) noexcept
    : address_(address), size_bytes_(size_bytes), is_write_(is_write) {
    std::snprintf(message_.data(), message_.size(), "unmapped %s.%s at $%06X",
                  is_write ? "write" : "read", size_suffix(size_bytes), address);
}

MemoryBus::MemoryBus() noexcept { rebuild_banks(); }

void MemoryBus::load_kickstart(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open Kickstart ROM: " + path.string());
    }
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>()};
    if (file.bad()) {
        throw std::runtime_error("error reading Kickstart ROM: " + path.string());
    }
    load_kickstart(image);
}

void MemoryBus::load_kickstart(std::span<const uint8_t> image) {
    // Cloanto "Amiga Forever" images are encrypted and need rom.key.
    static constexpr char kCloantoMagic[] = "AMIROMTYPE1";
    constexpr size_t kCloantoMagicLen = sizeof(kCloantoMagic) - 1;
    if (image.size() >= kCloantoMagicLen &&
        std::memcmp(image.data(), kCloantoMagic, kCloantoMagicLen) == 0) {
        throw std::runtime_error("encrypted Cloanto Kickstart images are not supported");
    }
    if (image.size() != kRomSize) {
        throw std::runtime_error("Kickstart ROM must be exactly 256KB (got " +
                                 std::to_string(image.size()) + " bytes)");
    }

    std::copy(image.begin(), image.end(), rom_.begin());
    rom_loaded_ = true;
    rebuild_banks();

    // 256KB Kickstarts (1.x) start with $1111 followed by a JMP opcode.
    if (read16(kRomBase) != 0x1111) {
        std::fprintf(stderr, "[bus] warning: Kickstart does not start with $1111 (got $%04X)\n",
                     read16(kRomBase));
    }
}

void MemoryBus::set_overlay(bool enabled) noexcept {
    if (overlay_ != enabled) {
        overlay_ = enabled;
        rebuild_banks();
    }
}

void MemoryBus::attach_custom_chips(CustomChipPort* port) noexcept {
    custom_ = port;
    rebuild_banks();
}

void MemoryBus::attach_cias(CiaPort* port) noexcept {
    cia_ = port;
    rebuild_banks();
}

void MemoryBus::note_open_bus(uint32_t address, bool is_write) const noexcept {
    bool& logged = open_bus_logged_[address >> kBankShift];
    if (logged) return;
    logged = true;
    std::fprintf(stderr, "[bus] %s empty space at $%06X (open bus)\n", is_write ? "write to" : "read from", address);
}

uint8_t MemoryBus::read_io8(const Bank& bank, uint32_t address) const {
    address &= kAddressMask;
    if (bank.open_bus) {
        note_open_bus(address, false);
        return static_cast<uint8_t>(kOpenBusValue);
    }
    if (bank.cia) {
        uint8_t value = 0;
        if (!cia_->read_cia(address, value)) unmapped_access(address, 1, false);
        return value;
    }
    // Custom chips: a byte read is one half of the word.
    const uint16_t word = read_custom(address & ~1u, 1);
    return static_cast<uint8_t>((address & 1u) != 0 ? word : word >> 8);
}

// A word access to the CIA area reaches CIA-B on the high byte (even address)
// and CIA-A on the low byte (odd address), each only if selected. A half no
// CIA answers reads as $FF (undriven); no CIA at all is a panic.
uint16_t MemoryBus::read_io16(const Bank& bank, uint32_t address) const {
    address &= kAddressMask;
    if (bank.open_bus) {
        note_open_bus(address, false);
        return kOpenBusValue;
    }
    if (!bank.cia) return read_custom(address, 2);
    uint8_t high = 0xFF;
    uint8_t low = 0xFF;
    const bool high_selected = cia_->read_cia(address, high);
    const bool low_selected = cia_->read_cia(address + 1, low);
    if (!high_selected && !low_selected) unmapped_access(address, 2, false);
    return static_cast<uint16_t>(high << 8 | low);
}

void MemoryBus::write_io8(const Bank& bank, uint32_t address, uint8_t value) const {
    address &= kAddressMask;
    if (bank.open_bus) {
        note_open_bus(address, true);
        return;
    }
    if (bank.cia) {
        if (!cia_->write_cia(address, value)) unmapped_access(address, 1, true);
        return;
    }
    // The 68000 drives a byte on both halves of the data bus and the custom
    // chips ignore UDS/LDS, so they see the byte twice.
    write_custom(address & ~1u, static_cast<uint16_t>(value << 8 | value), 1);
}

void MemoryBus::write_io16(const Bank& bank, uint32_t address, uint16_t value) const {
    address &= kAddressMask;
    if (bank.open_bus) {
        note_open_bus(address, true);
        return;
    }
    if (!bank.cia) {
        write_custom(address, value, 2);
        return;
    }
    const bool high_selected = cia_->write_cia(address, static_cast<uint8_t>(value >> 8));
    const bool low_selected = cia_->write_cia(address + 1, static_cast<uint8_t>(value));
    if (!high_selected && !low_selected) unmapped_access(address, 2, true);
}

// The custom chips decode A1-A8 only: the register block repeats every 512 bytes.
uint16_t MemoryBus::read_custom(uint32_t address, uint8_t) const {
    return custom_->read_custom(static_cast<uint16_t>(address & (kCustomSize - 2)));
}

void MemoryBus::write_custom(uint32_t address, uint16_t value, uint8_t) const {
    custom_->write_custom(static_cast<uint16_t>(address & (kCustomSize - 2)), value);
}

void MemoryBus::map(uint32_t start, uint32_t end, uint8_t* base, uint32_t mask,
                    bool writable) noexcept {
    for (uint32_t bank = start >> kBankShift; bank < (end >> kBankShift); ++bank) {
        banks_[bank] = Bank{base, mask, writable};
    }
}

void MemoryBus::rebuild_banks() noexcept {
    banks_.fill(Bank{});

    map(kChipRamBase, kChipRamWindowEnd, chip_ram_.data(), kChipRamSize - 1, true);
    map(kSlowRamBase, kSlowRamBase + kSlowRamSize, slow_ram_.data(), kSlowRamSize - 1, true);

    if (custom_ != nullptr) {
        // The 64KB bank holding $DFF000; only $DFF000-$DFF1FF is decoded
        // (checked in read_custom/write_custom).
        for (uint32_t bank = (kSlowRamBase + kSlowRamSize) >> kBankShift; bank <= (kCustomBase >> kBankShift); ++bank) {
            banks_[bank].custom = true;
        }
    }
    if (cia_ != nullptr) {
        banks_[kCiaBank >> kBankShift].cia = true;
    }
    for (uint32_t bank = 0xE8; bank < 0xF0; ++bank) banks_[bank].open_bus = true;  // Autoconfig
    for (uint32_t bank = 0xF0; bank < 0xF8; ++bank) banks_[bank].open_bus = true;  // diagnostic ROM

    if (rom_loaded_) {
        map(kRomWindowBase, kAddressMask + 1, rom_.data(), kRomSize - 1, false);
        if (overlay_) {
            // ROM replaces the first 512KB of the chip RAM window.
            map(kChipRamBase, kChipRamBase + kChipRamSize, rom_.data(), kRomSize - 1, false);
        }
    }
}

void MemoryBus::unmapped_access(uint32_t address, uint8_t size, bool is_write) {
    const BusError error(address, size, is_write);
    std::fprintf(stderr, "[bus] PANIC: %s\n", error.what());
    throw error;
}

void MemoryBus::ignored_rom_write(uint32_t address, uint8_t size) noexcept {
    std::fprintf(stderr, "[bus] warning: ignored write.%s to ROM at $%06X\n", size_suffix(size),
                 address & kAddressMask);
}

}  // namespace amiga
