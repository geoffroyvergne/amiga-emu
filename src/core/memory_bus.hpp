#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>

namespace amiga {

// Raised when the CPU touches an address no device decodes. This is a panic:
// it means either a bug in the emulator or hardware that isn't emulated yet.
class BusError final : public std::exception {
public:
    BusError(uint32_t address, uint8_t size_bytes, bool is_write) noexcept;

    [[nodiscard]] const char* what() const noexcept override { return message_.data(); }
    [[nodiscard]] uint32_t address() const noexcept { return address_; }
    [[nodiscard]] uint8_t size_bytes() const noexcept { return size_bytes_; }
    [[nodiscard]] bool is_write() const noexcept { return is_write_; }

private:
    uint32_t address_;
    uint8_t size_bytes_;
    bool is_write_;
    std::array<char, 64> message_{};
};

// Register interface of the custom chips (Agnus, Denise, Paula) as seen
// from the CPU. Offsets are relative to $DFF000 and always even: the custom
// chips only perform word accesses.
class CustomChipPort {
public:
    virtual uint16_t read_custom(uint16_t offset) = 0;
    virtual void write_custom(uint16_t offset, uint16_t value) = 0;

protected:
    ~CustomChipPort() = default;
};

// The two 8520 CIAs as seen from the CPU: byte registers, selected by
// address lines. Returns false when no CIA is selected at that address.
class CiaPort {
public:
    virtual bool read_cia(uint32_t address, uint8_t& value) = 0;
    virtual bool write_cia(uint32_t address, uint8_t value) = 0;

protected:
    ~CiaPort() = default;
};

// Amiga 500 (OCS) address decoding, as seen by the 68000.
//
// The 68000 has a 24-bit address bus, so A24-A31 are ignored. All multi-byte
// accesses are big-endian: the byte at the lower address is the most
// significant one.
//
//   $000000-$1FFFFF  Chip RAM, 512KB, repeated 4 times in this window
//                    (the A500 only decodes A0-A18 for chip RAM; Kickstart
//                    relies on that repetition to size chip memory).
//   $C00000-$C7FFFF  slow RAM, 512KB.
//   $BF0000-$BFFFFF  CIA-A (odd addresses, A12 = 0) and CIA-B (even, A13 = 0),
//                    once attach_cias() is called.
//   $C80000-$DFFFFF  custom chip registers, once attach_custom_chips() is
//                    called: the A500 selects the custom chips for this whole
//                    range where no slow RAM answers, and they only see A1-A8,
//                    so the 512-byte register block repeats. $DFF000 is the
//                    canonical copy; Kickstart relies on the others to size
//                    slow RAM.
//   $E80000-$EFFFFF  Zorro II Autoconfig space, empty (no expansion boards):
//                    open bus, reads 0, writes ignored.
//   $F00000-$F7FFFF  empty on the A500 (diagnostic cartridge space that
//                    Kickstart probes): open bus.
//   $F80000-$FFFFFF  Kickstart ROM, 256KB, at $FC0000 and repeated at $F80000.
//   everything else  unmapped for now -> BusError (slow RAM is still to be
//                    emulated).
//
// Overlay (CIA-A PRA bit 0, OVL): at reset the ROM also appears at $000000
// in place of chip RAM, so the CPU can fetch its reset vectors
// (SSP at $000000, PC at $000004). Kickstart clears OVL early in boot.
// Until a ROM is loaded, the overlay has no effect.
//
// The object holds 1.25MB inline; create it on the heap.
class MemoryBus {
public:
    static constexpr uint32_t kAddressMask = 0x00FF'FFFF;

    static constexpr uint32_t kChipRamBase = 0x00'0000;
    static constexpr uint32_t kChipRamSize = 512 * 1024;
    static constexpr uint32_t kChipRamWindowEnd = 0x20'0000;  // exclusive

    // A501-style "trapdoor" expansion: 512KB of slow RAM (on the chip bus,
    // but not reachable by DMA).
    static constexpr uint32_t kSlowRamBase = 0xC0'0000;
    static constexpr uint32_t kSlowRamSize = 512 * 1024;

    static constexpr uint32_t kRomBase = 0xFC'0000;
    static constexpr uint32_t kRomSize = 256 * 1024;
    static constexpr uint32_t kRomWindowBase = 0xF8'0000;  // $F80000-$FFFFFF

    static constexpr uint32_t kCiaBank = 0xBF'0000;
    // Nothing drives the data bus there: the value is undefined on hardware.
    // 0 is what Kickstart must see: "no board" in empty Autoconfig space, and
    // not the $1111 diagnostic ROM signature.
    static constexpr uint16_t kOpenBusValue = 0x0000;
    static constexpr uint32_t kCustomBase = 0xDF'F000;
    static constexpr uint32_t kCustomSize = 0x200;

    MemoryBus() noexcept;

    // Loads a 256KB Kickstart image. Throws std::runtime_error on failure;
    // the bus keeps its previous ROM contents in that case.
    void load_kickstart(const std::filesystem::path& path);
    void load_kickstart(std::span<const uint8_t> image);
    [[nodiscard]] bool kickstart_loaded() const noexcept { return rom_loaded_; }

    // Driven by CIA-A PRA bit 0. True at power-on / reset.
    void set_overlay(bool enabled) noexcept;
    [[nodiscard]] bool overlay() const noexcept { return overlay_; }

    // Maps the custom chip registers at $DFF000. The port must outlive the bus.
    void attach_custom_chips(CustomChipPort* port) noexcept;
    // Maps the CIAs in the $BF0000 bank. The port must outlive the bus.
    void attach_cias(CiaPort* port) noexcept;

    // Puts the bus in its reset state (overlay on). RAM contents are
    // preserved, as on real hardware.
    void reset() noexcept { set_overlay(true); }

    [[nodiscard]] uint8_t read8(uint32_t address) const {
        const auto [bank, offset] = resolve(address, 1, false);
        if (bank.base == nullptr) return read_io8(bank, address);
        return bank.base[offset];
    }

    // Word and long accesses must be at even addresses: the 68000 raises
    // an address error before the bus is involved, so the CPU checks that.
    // Odd addresses are still handled here (byte by byte) so the bus stays
    // well defined for debuggers and tests.
    [[nodiscard]] uint16_t read16(uint32_t address) const {
        if ((address & 1u) != 0) {
            return static_cast<uint16_t>((read8(address) << 8) | read8(address + 1));
        }
        const auto [bank, offset] = resolve(address, 2, false);
        if (bank.base == nullptr) return read_io16(bank, address);
        const uint8_t* p = bank.base + offset;
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    // The 68000 performs a long access as two word bus cycles, high word first.
    [[nodiscard]] uint32_t read32(uint32_t address) const {
        const uint32_t hi = read16(address);
        const uint32_t lo = read16(address + 2);
        return (hi << 16) | lo;
    }

    void write8(uint32_t address, uint8_t value) {
        const auto [bank, offset] = resolve(address, 1, true);
        if (bank.base == nullptr) {
            write_io8(bank, address, value);
        } else if (bank.writable) {
            bank.base[offset] = value;
        } else {
            ignored_rom_write(address, 1);
        }
    }

    void write16(uint32_t address, uint16_t value) {
        if ((address & 1u) != 0) {
            write8(address, static_cast<uint8_t>(value >> 8));
            write8(address + 1, static_cast<uint8_t>(value));
            return;
        }
        const auto [bank, offset] = resolve(address, 2, true);
        if (bank.base == nullptr) {
            write_io16(bank, address, value);
        } else if (bank.writable) {
            uint8_t* p = bank.base + offset;
            p[0] = static_cast<uint8_t>(value >> 8);
            p[1] = static_cast<uint8_t>(value);
        } else {
            ignored_rom_write(address, 2);
        }
    }

    void write32(uint32_t address, uint32_t value) {
        write16(address, static_cast<uint16_t>(value >> 16));
        write16(address + 2, static_cast<uint16_t>(value));
    }

    // Debugger read: RAM/ROM only, no side effects. I/O and unmapped
    // addresses return nullopt.
    [[nodiscard]] std::optional<uint16_t> peek16(uint32_t address) const noexcept {
        address &= kAddressMask & ~1u;
        const Bank& bank = banks_[address >> kBankShift];
        if (bank.base == nullptr) return std::nullopt;
        const uint8_t* p = bank.base + (address & bank.mask);
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    // Direct access for DMA (Agnus), debuggers and tests.
    [[nodiscard]] std::span<uint8_t, kChipRamSize> chip_ram() noexcept { return chip_ram_; }
    [[nodiscard]] std::span<const uint8_t, kChipRamSize> chip_ram() const noexcept { return chip_ram_; }

private:
    static constexpr uint32_t kBankShift = 16;  // 64KB banks
    static constexpr size_t kBankCount = (kAddressMask >> kBankShift) + 1;  // 256

    struct Bank {
        uint8_t* base = nullptr;  // nullptr = unmapped, or I/O if `custom` / `cia`
        uint32_t mask = 0;        // offset = address & mask (handles mirroring)
        bool writable = false;
        bool custom = false;
        bool cia = false;
        bool open_bus = false;  // nothing decoded, but probed by system software
    };

    struct Resolved {
        const Bank& bank;
        uint32_t offset;
    };

    // Word accesses at even addresses never straddle a bank: banks are 64KB
    // aligned and every mapping is a multiple of 64KB.
    [[nodiscard]] Resolved resolve(uint32_t address, uint8_t size, bool is_write) const {
        address &= kAddressMask;
        const Bank& bank = banks_[address >> kBankShift];
        if (bank.base == nullptr && !bank.custom && !bank.cia && !bank.open_bus) [[unlikely]] {
            unmapped_access(address, size, is_write);
        }
        return {bank, address & bank.mask};
    }

    void rebuild_banks() noexcept;
    void note_open_bus(uint32_t address, bool is_write) const noexcept;
    void map(uint32_t start, uint32_t end, uint8_t* base, uint32_t mask, bool writable) noexcept;

    // I/O banks (custom chips, CIAs, open bus). Panic where no CIA is selected.
    [[nodiscard]] uint8_t read_io8(const Bank& bank, uint32_t address) const;
    [[nodiscard]] uint16_t read_io16(const Bank& bank, uint32_t address) const;
    void write_io8(const Bank& bank, uint32_t address, uint8_t value) const;
    void write_io16(const Bank& bank, uint32_t address, uint16_t value) const;
    [[nodiscard]] uint16_t read_custom(uint32_t address, uint8_t size) const;
    void write_custom(uint32_t address, uint16_t value, uint8_t size) const;

    [[noreturn]] static void unmapped_access(uint32_t address, uint8_t size, bool is_write);
    static void ignored_rom_write(uint32_t address, uint8_t size) noexcept;

    std::array<uint8_t, kChipRamSize> chip_ram_{};
    std::array<uint8_t, kSlowRamSize> slow_ram_{};
    std::array<uint8_t, kRomSize> rom_{};
    std::array<Bank, kBankCount> banks_{};
    CustomChipPort* custom_ = nullptr;
    CiaPort* cia_ = nullptr;
    mutable std::array<bool, kBankCount> open_bus_logged_{};
    bool rom_loaded_ = false;
    bool overlay_ = true;
};

}  // namespace amiga
