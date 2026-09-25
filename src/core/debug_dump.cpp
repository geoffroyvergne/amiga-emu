#include "core/debug_dump.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

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
        std::fprintf(out, "  %08X  %04X  %-40s%s\n", entry.pc, entry.opcode, dis.text.c_str(), changes.c_str());
    }
    std::fprintf(out, "  %08X  %04X  %-40s<- next\n", cpu.pc(), bus.peek16(cpu.pc()).value_or(0),
                 disassemble(bus, cpu.pc()).text.c_str());

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
    const bool double_fault = halt.find("double bus fault") != std::string::npos;
    const bool bus_error = halt.find("unmapped") != std::string::npos;
    std::fprintf(out, "  faults: address errors=%llu, illegal opcodes=%llu (line A=%llu, line F=%llu), "
                      "privilege violations=%llu, unmapped access=%s, double bus fault=%s\n",
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorAddressError)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorIllegal)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorLineA)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorLineF)),
                 static_cast<unsigned long long>(cpu.exception_count(Cpu68000::kVectorPrivilege)),
                 bus_error ? "YES (halted)" : "no", double_fault ? "YES (halted)" : "no");

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

namespace {

// Addresses an instruction's text refers to: absolute "$XXXXXXXX" and
// register-relative "[-]$D(An)" / "(An)" forms, resolved with the current
// register values.
std::vector<uint32_t> operand_addresses(const std::string& text, const Cpu68000& cpu) {
    std::vector<uint32_t> addresses;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(' && i + 2 < text.size() && (text[i + 1] == 'A' || text[i + 1] == 'S')) {
            const unsigned reg = text[i + 1] == 'S' ? 7u : static_cast<unsigned>(text[i + 2] - '0');
            int32_t displacement = 0;
            size_t j = i;
            while (j > 0 && std::isxdigit(static_cast<unsigned char>(text[j - 1]))) --j;
            if (j > 0 && text[j - 1] == '$') {
                displacement = static_cast<int32_t>(std::stoul(text.substr(j, i - j), nullptr, 16));
                if (j > 1 && text[j - 2] == '-') displacement = -displacement;
            }
            if (reg < 8) addresses.push_back(cpu.a(reg) + static_cast<uint32_t>(displacement));
        } else if (text[i] == '$' && i + 9 <= text.size() && text[i + 9 - 1] != '(' &&
                   std::all_of(text.begin() + static_cast<long>(i) + 1, text.begin() + static_cast<long>(i) + 9,
                               [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }) &&
                   (i + 9 == text.size() || text[i + 9] != '(')) {
            if (i == 0 || text[i - 1] != '#') addresses.push_back(static_cast<uint32_t>(std::stoul(text.substr(i + 1, 8), nullptr, 16)));
        }
    }
    return addresses;
}

std::string describe_address(uint32_t address, std::string& hint) {
    address &= 0xFF'FFFFu;
    if (address >= 0xC8'0000 && address < 0xE0'0000) {  // custom chips (and their mirrors)
        const auto offset = static_cast<uint16_t>(address & 0x1FEu);
        switch (offset) {
            case 0x002: hint = "polling DMACONR: probably waiting for the blitter (BBUSY) or a DMA state"; break;
            case 0x004: case 0x006: hint = "polling the beam position: waiting for a raster line"; break;
            case 0x01E: hint = "polling INTREQR: waiting for an interrupt request to be raised"; break;
            case 0x01A: hint = "polling DSKBYTR: waiting for disk data"; break;
            case 0x00A: case 0x00C: hint = "polling a joystick/mouse counter"; break;
            case 0x016: hint = "polling POTGOR: waiting for the right mouse button / 2nd fire button"; break;
            default: break;
        }
        return custom_register_name(offset);
    }
    if ((address & 0xFF'0000u) == 0xBF'0000u) {
        static constexpr std::array<const char*, 16> kRegs = {"PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
                                                              "TODLO", "TODMID", "TODHI", "?", "SDR", "ICR", "CRA", "CRB"};
        const bool cia_a = (address & 1u) != 0;
        const unsigned reg = (address >> 8) & 0xFu;
        if (cia_a && reg == 0) hint = "polling CIA-A PRA: waiting for a mouse/fire button (or a disk status line)";
        if (reg == 0xD) hint = "polling a CIA ICR: waiting for a CIA timer/keyboard/index interrupt flag";
        if (reg >= 4 && reg <= 0xA) hint = "polling a CIA timer / time-of-day counter";
        return std::string(cia_a ? "CIA-A " : "CIA-B ") + kRegs[reg];
    }
    return format("$%06X", address);
}

void print_bits(std::FILE* out, const char* name, uint16_t value, const std::array<const char*, 16>& names) {
    std::fprintf(out, "  %-8s = $%04X:", name, value);
    for (int bit = 15; bit >= 0; --bit) {
        if ((value & (1u << bit)) != 0 && names[static_cast<size_t>(bit)][0] != '\0') {
            std::fprintf(out, " %s", names[static_cast<size_t>(bit)]);
        }
    }
    std::fprintf(out, "\n");
}

}  // namespace

void dump_deadlock(Machine& machine, const Machine::Deadlock& deadlock, std::FILE* out) {
    const MemoryBus& bus = machine.bus();
    const Cpu68000& cpu = machine.cpu();
    Chipset& chipset = machine.chipset();

    std::fprintf(out, "\n*** DEADLOCK WARNING at frame %llu: the CPU has run the same loop %u times in a row ***\n",
                 static_cast<unsigned long long>(deadlock.frame), Machine::kDeadlockIterations);
    std::string hint;
    std::vector<std::string> polled;
    const uint32_t first = std::min(deadlock.pc_a, deadlock.pc_b);
    const uint32_t last = std::max(deadlock.pc_a, deadlock.pc_b);
    for (uint32_t pc = first;; pc = last) {
        const Disassembly dis = disassemble(bus, pc);
        std::fprintf(out, "  loop: %08X  %04X  %s\n", pc, bus.peek16(pc).value_or(0), dis.text.c_str());
        // Branch/jump operands are targets, not data.
        const std::string& t = dis.text;
        const bool flow = t.rfind("DB", 0) == 0 || t.rfind("JMP", 0) == 0 || t.rfind("JSR", 0) == 0 ||
                          (t[0] == 'B' && t.rfind("BTST", 0) != 0 && t.rfind("BCHG", 0) != 0 &&
                           t.rfind("BCLR", 0) != 0 && t.rfind("BSET", 0) != 0);
        if (!flow) {
            for (const uint32_t address : operand_addresses(t, cpu)) polled.push_back(describe_address(address, hint));
        }
        if (pc == last) break;
    }
    if (!polled.empty()) {
        std::fprintf(out, "  reads/writes:");
        for (const std::string& p : polled) std::fprintf(out, " %s", p.c_str());
        std::fprintf(out, "\n");
    }
    std::fprintf(out, "  likely cause: %s\n",
                 hint.empty() ? "a busy-wait on memory or a flag set by an interrupt handler (see the loop)" : hint.c_str());

    uint16_t dmaconr = 0;
    chipset.agnus().read_register(reg::kDmaConR, dmaconr);
    dmaconr = static_cast<uint16_t>(dmaconr | (chipset.blitter().zero() ? 0x2000u : 0u));
    static constexpr std::array<const char*, 16> kDma = {"AUD0EN", "AUD1EN", "AUD2EN", "AUD3EN", "DSKEN", "SPREN", "BLTEN", "COPEN",
                                                         "BPLEN",  "DMAEN",  "BLTPRI", "",      "",      "BZERO", "BBUSY", ""};
    static constexpr std::array<const char*, 16> kInt = {"TBE", "DSKBLK", "SOFT", "PORTS", "COPER", "VERTB", "BLIT", "AUD0",
                                                         "AUD1", "AUD2", "AUD3", "RBF", "DSKSYN", "EXTER", "INTEN", ""};
    print_bits(out, "DMACONR", dmaconr, kDma);
    std::fprintf(out, "             BBUSY=%d (blits complete when started in this emulator, so a BBUSY wait never hangs)\n",
                 (dmaconr & 0x4000u) != 0);
    const Paula& paula = chipset.paula();
    print_bits(out, "INTENA", paula.intena(), kInt);
    print_bits(out, "INTREQ", paula.intreq(), kInt);
    const auto pending_disabled = static_cast<uint16_t>(paula.intreq() & ~paula.intena() & 0x3FFFu);
    if (pending_disabled != 0) print_bits(out, "requested, not enabled", pending_disabled, kInt);
    if ((paula.intena() & reg::kIntEn) == 0) std::fprintf(out, "  interrupts are disabled (INTEN clear)\n");
    std::fprintf(out, "  CPU: PC=%08X SR=%04X (interrupt mask %u), CPU interrupt level from Paula: %u\n", cpu.pc(), cpu.sr(),
                 cpu.interrupt_mask(), chipset.interrupt_level());
    if (chipset.interrupt_level() != 0 && chipset.interrupt_level() <= cpu.interrupt_mask()) {
        std::fprintf(out, "  NOTE: a level %u interrupt is pending but the CPU's SR mask (%u) blocks it\n",
                     chipset.interrupt_level(), cpu.interrupt_mask());
    }
    static constexpr std::array<const char*, 5> kCopperState = {"stopped", "fetch IR1", "fetch IR2", "waiting", "wake"};
    std::fprintf(out, "  Copper: %s at $%06X, beam V=%03X H=%02X\n",
                 kCopperState[static_cast<size_t>(chipset.copper().state())], chipset.copper().pc(),
                 chipset.agnus().vpos(), chipset.agnus().hpos());
    Cias& cias = machine.cias();
    std::fprintf(out, "  CIA-A ICR=%02X mask=%02X  CIA-B ICR=%02X mask=%02X\n", cias.a().peek(Cia8520::kIcr),
                 cias.a().icr_mask(), cias.b().peek(Cia8520::kIcr), cias.b().icr_mask());
}

}  // namespace amiga
