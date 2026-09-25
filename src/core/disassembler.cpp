#include "core/disassembler.hpp"

#include <array>
#include <cstdio>

#include "core/cpu68000.hpp"

namespace amiga {

namespace {

constexpr std::array<const char*, 16> kConditions = {"T",  "F",  "HI", "LS", "CC", "CS", "NE", "EQ",
                                                     "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"};

std::string format(const char* fmt, auto... args) {
    std::array<char, 96> buffer{};
    std::snprintf(buffer.data(), buffer.size(), fmt, args...);
    return buffer.data();
}

std::string dreg(unsigned n) { return format("D%u", n); }
std::string areg(unsigned n) { return n == 7 ? "SP" : format("A%u", n); }

std::string signed_hex(int32_t value) {
    return value < 0 ? format("-$%X", static_cast<unsigned>(-value)) : format("$%X", static_cast<unsigned>(value));
}

class Reader {
public:
    Reader(const MemoryBus& bus, uint32_t pc) : bus_(bus), pc_(pc) {}

    uint16_t word() {
        const auto w = bus_.peek16(pc_);
        pc_ += 2;
        return w.value_or(0);
    }
    uint32_t pc() const { return pc_; }

private:
    const MemoryBus& bus_;
    uint32_t pc_;
};

// Size field used by most instructions (bits 7-6).
char size_char(unsigned bits) { return bits == 0 ? 'B' : bits == 1 ? 'W' : 'L'; }

std::string index_suffix(uint16_t ext) {
    const unsigned reg = (ext >> 12) & 7u;
    return format("%s.%c", ((ext & 0x8000u) != 0 ? areg(reg) : dreg(reg)).c_str(), (ext & 0x0800u) != 0 ? 'L' : 'W');
}

std::string ea(Reader& r, unsigned mode, unsigned reg, char size) {
    switch (mode) {
        case 0: return dreg(reg);
        case 1: return areg(reg);
        case 2: return "(" + areg(reg) + ")";
        case 3: return "(" + areg(reg) + ")+";
        case 4: return "-(" + areg(reg) + ")";
        case 5: return signed_hex(static_cast<int16_t>(r.word())) + "(" + areg(reg) + ")";
        case 6: {
            const uint16_t ext = r.word();
            return signed_hex(static_cast<int8_t>(ext & 0xFFu)) + "(" + areg(reg) + "," + index_suffix(ext) + ")";
        }
        default: break;
    }
    switch (reg) {
        case 0: return format("$%04X.W", r.word());
        case 1: {
            const uint32_t hi = r.word();
            return format("$%08X", hi << 16 | r.word());
        }
        case 2: {
            const uint32_t base = r.pc();
            const auto disp = static_cast<int16_t>(r.word());
            return format("$%08X(PC)", base + static_cast<uint32_t>(disp));
        }
        case 3: {
            const uint32_t base = r.pc();
            const uint16_t ext = r.word();
            return format("$%08X(PC,%s)", base + static_cast<uint32_t>(static_cast<int8_t>(ext & 0xFFu)),
                          index_suffix(ext).c_str());
        }
        case 4: {
            if (size == 'L') {
                const uint32_t hi = r.word();
                return format("#$%08X", hi << 16 | r.word());
            }
            const uint16_t w = r.word();
            return size == 'B' ? format("#$%02X", w & 0xFFu) : format("#$%04X", w);
        }
        default: return "?";
    }
}

std::string ea_of(Reader& r, uint16_t op, char size) { return ea(r, (op >> 3) & 7u, op & 7u, size); }

std::string op_sized(const char* name, char size) { return format("%s.%c", name, size); }

std::string register_list(uint16_t mask, bool reversed) {
    std::string out;
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned bit = reversed ? 15 - i : i;
        if ((mask & (1u << bit)) == 0) continue;
        if (!out.empty()) out += "/";
        out += i < 8 ? dreg(i) : areg(i - 8);
    }
    return out.empty() ? "0" : out;
}

std::string two(const std::string& mnemonic, const std::string& a, const std::string& b) {
    return format("%-8s", mnemonic.c_str()) + a + "," + b;
}
std::string one(const std::string& mnemonic, const std::string& a) { return format("%-8s", mnemonic.c_str()) + a; }

std::string decode(Reader& r, uint16_t op) {
    const unsigned reg9 = (op >> 9) & 7u;
    const unsigned mode = (op >> 3) & 7u;
    const unsigned reg = op & 7u;
    const unsigned size_bits = (op >> 6) & 3u;
    const char sz = size_char(size_bits);

    switch (op >> 12) {
        case 0x0: {
            if ((op & 0xF138u) == 0x0108u) {  // MOVEP
                const char s = (op & 0x40u) != 0 ? 'L' : 'W';
                const std::string mem = signed_hex(static_cast<int16_t>(r.word())) + "(" + areg(reg) + ")";
                return (op & 0x80u) != 0 ? two(op_sized("MOVEP", s), dreg(reg9), mem)
                                         : two(op_sized("MOVEP", s), mem, dreg(reg9));
            }
            static constexpr std::array<const char*, 4> kBit = {"BTST", "BCHG", "BCLR", "BSET"};
            if ((op & 0x0100u) != 0) return two(kBit[size_bits], dreg(reg9), ea_of(r, op, 'B'));
            if ((op & 0xFF00u) == 0x0800u) {
                const std::string bit = format("#%u", r.word() & 0xFFu);
                return two(kBit[size_bits], bit, ea_of(r, op, 'B'));
            }
            static constexpr std::array<const char*, 8> kImm = {"ORI", "ANDI", "SUBI", "ADDI", "?", "EORI", "CMPI", "?"};
            if ((op & 0x00FFu) == 0x003Cu) return two(kImm[reg9], format("#$%02X", r.word() & 0xFFu), "CCR");
            if ((op & 0x00FFu) == 0x007Cu) return two(kImm[reg9], format("#$%04X", r.word()), "SR");
            const std::string imm = ea(r, 7, 4, sz);
            return two(op_sized(kImm[reg9], sz), imm, ea_of(r, op, sz));
        }
        case 0x1:
        case 0x2:
        case 0x3: {
            const char s = (op >> 12) == 1 ? 'B' : (op >> 12) == 3 ? 'W' : 'L';
            const std::string src = ea_of(r, op, s);
            const unsigned dst_mode = (op >> 6) & 7u;
            if (dst_mode == 1) return two(op_sized("MOVEA", s), src, areg(reg9));
            return two(op_sized("MOVE", s), src, ea(r, dst_mode, reg9, s));
        }
        case 0x4: {
            switch (op) {
                case 0x4AFC: return "ILLEGAL";
                case 0x4E70: return "RESET";
                case 0x4E71: return "NOP";
                case 0x4E72: return one("STOP", format("#$%04X", r.word()));
                case 0x4E73: return "RTE";
                case 0x4E75: return "RTS";
                case 0x4E76: return "TRAPV";
                case 0x4E77: return "RTR";
                default: break;
            }
            if ((op & 0xFFF0u) == 0x4E40u) return one("TRAP", format("#%u", op & 0xFu));
            if ((op & 0xFFF8u) == 0x4E50u) return two("LINK", areg(reg), format("#%s", signed_hex(static_cast<int16_t>(r.word())).c_str()));
            if ((op & 0xFFF8u) == 0x4E58u) return one("UNLK", areg(reg));
            if ((op & 0xFFF0u) == 0x4E60u) return (op & 8u) != 0 ? two("MOVE", "USP", areg(reg)) : two("MOVE", areg(reg), "USP");
            if ((op & 0xFFC0u) == 0x4E80u) return one("JSR", ea_of(r, op, 'L'));
            if ((op & 0xFFC0u) == 0x4EC0u) return one("JMP", ea_of(r, op, 'L'));
            if ((op & 0xFFB8u) == 0x4880u) return one((op & 0x40u) != 0 ? "EXT.L" : "EXT.W", dreg(reg));
            if ((op & 0xFFF8u) == 0x4840u) return one("SWAP", dreg(reg));
            if ((op & 0xFFC0u) == 0x4840u) return one("PEA", ea_of(r, op, 'L'));
            if ((op & 0xFB80u) == 0x4880u) {
                const char s = (op & 0x40u) != 0 ? 'L' : 'W';
                const uint16_t mask = r.word();
                if ((op & 0x0400u) != 0) return two(op_sized("MOVEM", s), ea_of(r, op, s), register_list(mask, false));
                return two(op_sized("MOVEM", s), register_list(mask, mode == 4), ea_of(r, op, s));
            }
            if ((op & 0xFFC0u) == 0x4800u) return one("NBCD", ea_of(r, op, 'B'));
            if ((op & 0xFFC0u) == 0x40C0u) return two("MOVE", "SR", ea_of(r, op, 'W'));
            if ((op & 0xFFC0u) == 0x44C0u) return two("MOVE", ea_of(r, op, 'W'), "CCR");
            if ((op & 0xFFC0u) == 0x46C0u) return two("MOVE", ea_of(r, op, 'W'), "SR");
            if ((op & 0xFFC0u) == 0x4AC0u) return one("TAS", ea_of(r, op, 'B'));
            if ((op & 0xFF00u) == 0x4A00u) return one(op_sized("TST", sz), ea_of(r, op, sz));
            if ((op & 0xF1C0u) == 0x4180u) return two("CHK.W", ea_of(r, op, 'W'), dreg(reg9));
            if ((op & 0xF1C0u) == 0x41C0u) return two("LEA", ea_of(r, op, 'L'), areg(reg9));
            static constexpr std::array<const char*, 4> kSingle = {"NEGX", "CLR", "NEG", "NOT"};
            return one(op_sized(kSingle[(op >> 9) & 3u], sz), ea_of(r, op, sz));
        }
        case 0x5: {
            const unsigned cc = (op >> 8) & 0xFu;
            if ((op & 0x00F8u) == 0x00C8u) {
                const uint32_t base = r.pc();
                const auto disp = static_cast<int16_t>(r.word());
                return two(std::string("DB") + (cc == 1 ? "RA" : kConditions[cc]), dreg(reg),
                           format("$%08X", base + static_cast<uint32_t>(disp)));
            }
            if (size_bits == 3) return one(std::string("S") + kConditions[cc], ea_of(r, op, 'B'));
            const unsigned data = reg9 == 0 ? 8 : reg9;
            return two(op_sized((op & 0x100u) != 0 ? "SUBQ" : "ADDQ", sz), format("#%u", data), ea_of(r, op, sz));
        }
        case 0x6: {
            const unsigned cc = (op >> 8) & 0xFu;
            const uint32_t base = r.pc();
            int32_t disp = static_cast<int8_t>(op & 0xFFu);
            const bool word = disp == 0;
            if (word) disp = static_cast<int16_t>(r.word());
            const std::string name = cc == 0 ? "BRA" : cc == 1 ? "BSR" : std::string("B") + kConditions[cc];
            return one(name + (word ? ".W" : ".S"), format("$%08X", base + static_cast<uint32_t>(disp)));
        }
        case 0x7: return two("MOVEQ", format("#%d", static_cast<int8_t>(op & 0xFFu)), dreg(reg9));
        case 0x8:
        case 0xC: {
            const bool is_and = (op >> 12) == 0xC;
            const unsigned om = (op >> 6) & 7u;
            if (om == 3 || om == 7) {
                const char* name = is_and ? ((om == 7) ? "MULS" : "MULU") : ((om == 7) ? "DIVS" : "DIVU");
                return two(std::string(name) + ".W", ea_of(r, op, 'W'), dreg(reg9));
            }
            if ((op & 0x01F0u) == 0x0100u) {
                const char* name = is_and ? "ABCD" : "SBCD";
                return (op & 8u) != 0 ? two(name, "-(" + areg(reg) + ")", "-(" + areg(reg9) + ")")
                                      : two(name, dreg(reg), dreg(reg9));
            }
            if (is_and && (op & 0x01F8u) == 0x0140u) return two("EXG", dreg(reg9), dreg(reg));
            if (is_and && (op & 0x01F8u) == 0x0148u) return two("EXG", areg(reg9), areg(reg));
            if (is_and && (op & 0x01F8u) == 0x0188u) return two("EXG", dreg(reg9), areg(reg));
            const char* name = is_and ? "AND" : "OR";
            if (om < 3) return two(op_sized(name, sz), ea_of(r, op, sz), dreg(reg9));
            return two(op_sized(name, sz), dreg(reg9), ea_of(r, op, sz));
        }
        case 0x9:
        case 0xD: {
            const char* base = (op >> 12) == 0x9 ? "SUB" : "ADD";
            const unsigned om = (op >> 6) & 7u;
            if (om == 3 || om == 7) {
                const char s = om == 7 ? 'L' : 'W';
                return two(op_sized((std::string(base) + "A").c_str(), s), ea_of(r, op, s), areg(reg9));
            }
            if ((op & 0x0130u) == 0x0100u) {
                const std::string name = op_sized((std::string(base) + "X").c_str(), sz);
                return (op & 8u) != 0 ? two(name, "-(" + areg(reg) + ")", "-(" + areg(reg9) + ")")
                                      : two(name, dreg(reg), dreg(reg9));
            }
            if (om < 3) return two(op_sized(base, sz), ea_of(r, op, sz), dreg(reg9));
            return two(op_sized(base, sz), dreg(reg9), ea_of(r, op, sz));
        }
        case 0xB: {
            const unsigned om = (op >> 6) & 7u;
            if (om == 3 || om == 7) {
                const char s = om == 7 ? 'L' : 'W';
                return two(op_sized("CMPA", s), ea_of(r, op, s), areg(reg9));
            }
            if (om < 3) return two(op_sized("CMP", sz), ea_of(r, op, sz), dreg(reg9));
            if (mode == 1) return two(op_sized("CMPM", sz), "(" + areg(reg) + ")+", "(" + areg(reg9) + ")+");
            return two(op_sized("EOR", sz), dreg(reg9), ea_of(r, op, sz));
        }
        case 0xE: {
            static constexpr std::array<const char*, 4> kShift = {"AS", "LS", "ROX", "RO"};
            const char dir = (op & 0x100u) != 0 ? 'L' : 'R';
            if (size_bits == 3) {
                return one(format("%s%c.W", kShift[(op >> 9) & 3u], dir), ea_of(r, op, 'W'));
            }
            const std::string name = format("%s%c.%c", kShift[(op >> 3) & 3u], dir, sz);
            const std::string count = (op & 0x20u) != 0 ? dreg(reg9) : format("#%u", reg9 == 0 ? 8 : reg9);
            return two(name, count, dreg(reg));
        }
        case 0xA: return format("LINE-A  $%04X", op);
        default: return format("LINE-F  $%04X", op);
    }
}

}  // namespace

Disassembly disassemble(const MemoryBus& bus, uint32_t pc) {
    Reader reader(bus, pc);
    const auto first = bus.peek16(pc);
    if (!first) return {"<not readable>", 2};
    const uint16_t op = reader.word();
    if (!Cpu68000::is_implemented(op) && (op >> 12) != 0xA && (op >> 12) != 0xF) {
        return {format("DC.W    $%04X  ; illegal", op), 2};
    }
    std::string text = decode(reader, op);
    return {std::move(text), reader.pc() - pc};
}

}  // namespace amiga
