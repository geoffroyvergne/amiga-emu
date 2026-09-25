#include "core/debug_dump.hpp"

#include <array>

#include "core/cia.hpp"
#include "core/custom_registers.hpp"
#include "core/disassembler.hpp"

namespace amiga {

namespace {

std::string format(const char* fmt, auto... args) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), fmt, args...);
    return buffer.data();
}

const char* register_label(unsigned r) {
    static constexpr std::array<const char*, 16> kNames = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
                                                           "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};
    return kNames[r];
}

}  // namespace

std::string custom_register_name(uint16_t offset) {
    offset &= 0x1FEu;
    static constexpr std::array<const char*, 0x40> kLow = {
        "BLTDDAT", "DMACONR", "VPOSR",   "VHPOSR",  "DSKDATR", "JOY0DAT", "JOY1DAT", "CLXDAT",
        "ADKCONR", "POT0DAT", "POT1DAT", "POTGOR",  "SERDATR", "DSKBYTR", "INTENAR", "INTREQR",
        "DSKPTH",  "DSKPTL",  "DSKLEN",  "DSKDAT",  "REFPTR",  "VPOSW",   "VHPOSW",  "COPCON",
        "SERDAT",  "SERPER",  "POTGO",   "JOYTEST", "STREQU",  "STRVBL",  "STRHOR",  "STRLONG",
        "BLTCON0", "BLTCON1", "BLTAFWM", "BLTALWM", "BLTCPTH", "BLTCPTL", "BLTBPTH", "BLTBPTL",
        "BLTAPTH", "BLTAPTL", "BLTDPTH", "BLTDPTL", "BLTSIZE", "$05A",    "$05C",    "$05E",
        "BLTCMOD", "BLTBMOD", "BLTAMOD", "BLTDMOD", "$068",    "$06A",    "$06C",    "$06E",
        "BLTCDAT", "BLTBDAT", "BLTADAT", "$076",    "$078",    "$07A",    "$07C",    "DSKSYNC",
    };
    static constexpr std::array<const char*, 16> kCopper = {
        "COP1LCH", "COP1LCL", "COP2LCH", "COP2LCL", "COPJMP1", "COPJMP2", "COPINS", "DIWSTRT",
        "DIWSTOP", "DDFSTRT", "DDFSTOP", "DMACON",  "CLXCON",  "INTENA",  "INTREQ", "ADKCON",
    };
    static constexpr std::array<const char*, 8> kAudio = {"LCH", "LCL", "LEN", "PER", "VOL", "DAT", "?", "?"};
    static constexpr std::array<const char*, 4> kSprite = {"POS", "CTL", "DATA", "DATB"};

    if (offset < 0x080) return kLow[offset / 2];
    if (offset < 0x0A0) return kCopper[(offset - 0x080) / 2];
    if (offset < 0x0E0) return format("AUD%u%s", (offset - 0x0A0) / 16, kAudio[(offset & 0xFu) / 2]);
    if (offset < 0x0F8) return format("BPL%u%s", (offset - 0x0E0) / 4 + 1, (offset & 2u) != 0 ? "PTL" : "PTH");
    if (offset < 0x100) return format("$%03X", offset);
    if (offset < 0x108) {
        static constexpr std::array<const char*, 4> kBplCon = {"BPLCON0", "BPLCON1", "BPLCON2", "BPLCON3"};
        return kBplCon[(offset - 0x100) / 2];
    }
    if (offset == 0x108) return "BPL1MOD";
    if (offset == 0x10A) return "BPL2MOD";
    if (offset >= 0x110 && offset < 0x11C) return format("BPL%uDAT", (offset - 0x110) / 2 + 1);
    if (offset >= 0x120 && offset < 0x140) return format("SPR%uPT%c", (offset - 0x120) / 4, (offset & 2u) != 0 ? 'L' : 'H');
    if (offset >= 0x140 && offset < 0x180) return format("SPR%u%s", (offset - 0x140) / 8, kSprite[(offset & 7u) / 2]);
    if (offset >= 0x180 && offset < 0x1C0) return format("COLOR%02u", (offset - 0x180) / 2);
    return format("$%03X", offset);
}

void dump_state(Machine& machine, std::FILE* out, size_t instructions) {
    const MemoryBus& bus = machine.bus();
    const Cpu68000& cpu = machine.cpu();
    Chipset& chipset = machine.chipset();

    std::fprintf(out, "==================== emulator state ====================\n");
    std::fprintf(out, "frame %llu, CPU cycles %llu, CPU %s%s%s\n",
                 static_cast<unsigned long long>(machine.frame_count()),
                 static_cast<unsigned long long>(machine.cpu_cycles()),
                 machine.cpu_halted() ? "HALTED: " : "running", machine.halt_reason(),
                 cpu.stopped() ? " (STOP)" : "");

    // --- Instruction trace ---
    size_t available = 0;
    while (available < instructions && machine.trace(available) != nullptr) ++available;
    std::fprintf(out, "\n--- last %zu instructions (oldest first; registers changed by each) ---\n", available);
    for (size_t i = available; i-- > 0;) {
        const TraceEntry& entry = *machine.trace(i);
        const Disassembly dis = disassemble(bus, entry.pc);
        std::string changes;
        if (const TraceEntry* next = i > 0 ? machine.trace(i - 1) : nullptr) {
            for (unsigned r = 0; r < 16; ++r) {
                if (next->regs[r] != entry.regs[r]) changes += format(" %s=%08X", register_label(r), next->regs[r]);
            }
            if ((next->sr & 0xFF1Fu) != (entry.sr & 0xFF1Fu)) changes += format(" SR=%04X", next->sr);
        }
        std::fprintf(out, "  %08X  %-40s%s\n", entry.pc, dis.text.c_str(), changes.c_str());
    }
    std::fprintf(out, "  %08X  %-40s<- next\n", cpu.pc(), disassemble(bus, cpu.pc()).text.c_str());

    // --- CPU ---
    std::fprintf(out, "\n--- CPU ---\n");
    for (unsigned r = 0; r < 8; ++r) std::fprintf(out, "  D%u=%08X", r, cpu.d(r));
    std::fprintf(out, "\n");
    for (unsigned r = 0; r < 8; ++r) std::fprintf(out, "  A%u=%08X", r, cpu.a(r));
    std::fprintf(out, "\n  PC=%08X  SR=%04X  USP=%08X  SSP=%08X  IPL mask=%u\n", cpu.pc(), cpu.sr(), cpu.usp(),
                 cpu.ssp(), cpu.interrupt_mask());
    std::fprintf(out, "  exceptions taken:");
    bool any = false;
    for (unsigned v = 2; v < 64; ++v) {
        if (cpu.exception_count(static_cast<uint8_t>(v)) == 0) continue;
        std::fprintf(out, " vec%u=%llu", v, static_cast<unsigned long long>(cpu.exception_count(static_cast<uint8_t>(v))));
        any = true;
    }
    std::fprintf(out, "%s\n", any ? "" : " none");

    // Fault summary. Address errors are not stacked as exceptions: the CPU
    // halts on the first one (with the report above), so a double bus fault
    // (a fault while processing a group 0 exception) cannot happen.
    const std::string halt = machine.halt_reason();
    const bool address_error = halt.find("address error") != std::string::npos;
    const bool bus_error = halt.find("unmapped") != std::string::npos;
    std::fprintf(out, "  faults: illegal opcodes=%llu (line A=%llu, line F=%llu), privilege violations=%llu, "
                      "address error=%s, unmapped access=%s, double bus fault=no\n",
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorIllegal)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorLineA)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorLineF)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorPrivilege)),
                 address_error ? "YES (halted)" : "no", bus_error ? "YES (halted)" : "no");

    // --- Custom chips ---
    Agnus& agnus = chipset.agnus();
    Paula& paula = chipset.paula();
    uint16_t dmaconr = 0;
    agnus.read_register(reg::kDmaConR, dmaconr);
    std::fprintf(out, "\n--- custom chips ---\n");
    std::fprintf(out, "  beam V=%03X H=%02X  DMACONR=%04X  INTENAR=%04X  INTREQR=%04X  IPL=%u\n", agnus.vpos(),
                 agnus.hpos(), dmaconr, paula.intena(), paula.intreq(), chipset.interrupt_level());
    const Copper& copper = chipset.copper();
    static constexpr std::array<const char*, 5> kCopperState = {"stopped", "fetch IR1", "fetch IR2", "waiting", "wake"};
    std::fprintf(out, "  Copper: PC=%06X state=%s COP1LC=%06X COP2LC=%06X\n", copper.pc(),
                 kCopperState[static_cast<size_t>(copper.state())], copper.cop1lc(), copper.cop2lc());
    std::fprintf(out, "  registers written (last value):\n");
    unsigned column = 0;
    for (uint16_t offset = 0; offset < MemoryBus::kCustomSize; offset += 2) {
        if (!chipset.was_written(offset)) continue;
        std::fprintf(out, "    %-8s=%04X", custom_register_name(offset).c_str(), chipset.last_written(offset));
        if (++column % 6 == 0) std::fprintf(out, "\n");
    }
    if (column % 6 != 0) std::fprintf(out, "\n");

    // --- CIAs ---
    static constexpr std::array<const char*, 16> kCiaRegs = {"PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
                                                             "TODLO", "TODMID", "TODHI", "-", "SDR", "ICR", "CRA", "CRB"};
    Cias& cias = machine.cias();
    for (unsigned which = 0; which < 2; ++which) {
        const Cia8520& cia = which == 0 ? cias.a() : cias.b();
        std::fprintf(out, "\n--- CIA-%c --- ", which == 0 ? 'A' : 'B');
        for (unsigned r = 0; r < 16; ++r) {
            if (r == 0xB) continue;
            std::fprintf(out, "%s=%02X ", kCiaRegs[r], cia.peek(r));
        }
        std::fprintf(out, "ICRmask=%02X\n", cia.icr_mask());
    }
    std::fprintf(out, "  overlay %s\n", bus.overlay() ? "ON" : "off");
    std::fprintf(out, "=========================================================\n");
}

}  // namespace amiga
