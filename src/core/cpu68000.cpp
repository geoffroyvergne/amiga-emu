#include "core/cpu68000.hpp"

#include <bit>
#include <cstdio>
#include <utility>

namespace amiga {

namespace {

constexpr uint32_t sign_extend8(uint32_t value) noexcept {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(value & 0xFFu)));
}

constexpr uint32_t sign_extend16(uint32_t value) noexcept {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value & 0xFFFFu)));
}

// --- Effective address classes ------------------------------------------------
// The 12 addressing modes, indexed 0-11:
//   Dn An (An) (An)+ -(An) d16(An) d8(An,Xn) abs.W abs.L d16(PC) d8(PC,Xn) #imm
constexpr int ea_index(unsigned mode, unsigned reg) noexcept {
    if (mode < 7) return static_cast<int>(mode);
    return reg <= 4 ? static_cast<int>(7 + reg) : -1;
}

constexpr uint16_t ea_bit(int index) noexcept { return static_cast<uint16_t>(1u << index); }

constexpr uint16_t kEaAll = 0x0FFF;
constexpr uint16_t kEaData = kEaAll & ~ea_bit(1);
constexpr uint16_t kEaAlterable = 0x01FF;  // Dn through abs.L
constexpr uint16_t kEaDataAlterable = kEaAlterable & ~ea_bit(1);
constexpr uint16_t kEaMemoryAlterable = kEaDataAlterable & ~ea_bit(0);
constexpr uint16_t kEaControl =
    ea_bit(2) | ea_bit(5) | ea_bit(6) | ea_bit(7) | ea_bit(8) | ea_bit(9) | ea_bit(10);
constexpr uint16_t kEaControlAlterable = kEaControl & kEaAlterable;

constexpr bool ea_in(unsigned mode, unsigned reg, uint16_t modes) noexcept {
    const int index = ea_index(mode, reg);
    return index >= 0 && (modes & ea_bit(index)) != 0;
}

// Source/destination field in bits 5-0.
constexpr bool ea_ok(uint16_t opcode, uint16_t modes) noexcept {
    return ea_in((opcode >> 3) & 7u, opcode & 7u, modes);
}

constexpr unsigned ea_mode(uint16_t opcode) noexcept { return (opcode >> 3) & 7u; }
constexpr unsigned ea_reg(uint16_t opcode) noexcept { return opcode & 7u; }
constexpr unsigned reg_field(uint16_t opcode) noexcept { return (opcode >> 9) & 7u; }
constexpr unsigned opmode(uint16_t opcode) noexcept { return (opcode >> 6) & 7u; }
constexpr bool size_field_valid(uint16_t opcode) noexcept { return ((opcode >> 6) & 3u) != 3; }
constexpr bool is_register_or_immediate(unsigned mode, unsigned reg) noexcept {
    return mode <= 1 || (mode == 7 && reg == 4);
}

// Index into a per-mode cycle table (only valid for decodable modes).
constexpr size_t ea_slot(uint16_t opcode) noexcept {
    return static_cast<size_t>(ea_index(ea_mode(opcode), ea_reg(opcode)));
}

// Effective address calculation times (M68000 UM, table 8-1), by ea_index.
constexpr std::array<uint8_t, 12> kEaCyclesByteWord = {0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4};
constexpr std::array<uint8_t, 12> kEaCyclesLong = {0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8};

constexpr bool always(uint16_t) noexcept { return true; }

}  // namespace

// --- Errors -------------------------------------------------------------------

CpuError::CpuError(Kind kind, uint32_t pc, uint16_t opcode, uint32_t address) noexcept
    : kind_(kind), pc_(pc), opcode_(opcode), address_(address) {
    switch (kind) {
        case Kind::OddPcFetch:
            std::snprintf(message_.data(), message_.size(),
                          "address error: fetch from odd PC $%08X", pc);
            break;
        case Kind::OddDataAccess:
            std::snprintf(message_.data(), message_.size(),
                          "address error: odd access at $%08X by opcode $%04X at $%08X", address,
                          opcode, pc);
            break;
        case Kind::UnimplementedOpcode:
            std::snprintf(message_.data(), message_.size(), "unimplemented opcode $%04X at $%08X",
                          opcode, pc);
            break;
    }
}

// --- Dispatch table -----------------------------------------------------------

std::array<Cpu68000::Handler, 0x10000> Cpu68000::build_dispatch_table() {
    struct Pattern {
        uint16_t mask;
        uint16_t match;
        bool (*valid)(uint16_t);
        Handler handler;
    };

    // ADD/SUB/AND/OR share an opmode layout: 0-2 <ea>,Dn; 4-6 Dn,<ea>.
    constexpr auto add_sub_valid = [](uint16_t op) {
        const unsigned om = opmode(op);
        if (om == 3 || om == 7) return ea_ok(op, kEaAll);  // ADDA/SUBA
        if (om < 3) return ea_ok(op, kEaAll) && !(om == 0 && ea_mode(op) == 1);
        return ea_ok(op, kEaMemoryAlterable);  // Dn/An forms are ADDX/SUBX
    };
    constexpr auto and_or_valid = [](uint16_t op) {
        return opmode(op) < 3 ? ea_ok(op, kEaData) : ea_ok(op, kEaMemoryAlterable);
    };

    // First match wins, so more specific patterns come first.
    const Pattern patterns[] = {
        // Exact opcodes.
        {0xFFFF, 0x003C, always, &thunk<&Cpu68000::op_logic_to_ccr_sr>},  // ORI to CCR
        {0xFFFF, 0x007C, always, &thunk<&Cpu68000::op_logic_to_ccr_sr>},  // ORI to SR
        {0xFFFF, 0x023C, always, &thunk<&Cpu68000::op_logic_to_ccr_sr>},  // ANDI to CCR
        {0xFFFF, 0x027C, always, &thunk<&Cpu68000::op_logic_to_ccr_sr>},  // ANDI to SR
        {0xFFFF, 0x0A3C, always, &thunk<&Cpu68000::op_logic_to_ccr_sr>},  // EORI to CCR
        {0xFFFF, 0x0A7C, always, &thunk<&Cpu68000::op_logic_to_ccr_sr>},  // EORI to SR
        {0xFFFF, 0x4E70, always, &thunk<&Cpu68000::op_reset>},
        {0xFFFF, 0x4E71, always, &thunk<&Cpu68000::op_nop>},
        {0xFFFF, 0x4E72, always, &thunk<&Cpu68000::op_stop>},
        {0xFFFF, 0x4E73, always, &thunk<&Cpu68000::op_rte>},
        {0xFFFF, 0x4E75, always, &thunk<&Cpu68000::op_rts>},
        {0xFFFF, 0x4E76, always, &thunk<&Cpu68000::op_trapv>},
        {0xFFFF, 0x4E77, always, &thunk<&Cpu68000::op_rtr>},
        // Line 4: miscellaneous.
        {0xFFF0, 0x4E40, always, &thunk<&Cpu68000::op_trap>},
        {0xFFF8, 0x4E50, always, &thunk<&Cpu68000::op_link>},
        {0xFFF8, 0x4E58, always, &thunk<&Cpu68000::op_unlk>},
        {0xFFF0, 0x4E60, always, &thunk<&Cpu68000::op_move_usp>},
        {0xFFC0, 0x4E80, [](uint16_t op) { return ea_ok(op, kEaControl); }, &thunk<&Cpu68000::op_jsr>},
        {0xFFC0, 0x4EC0, [](uint16_t op) { return ea_ok(op, kEaControl); }, &thunk<&Cpu68000::op_jmp>},
        {0xFFF8, 0x4880, always, &thunk<&Cpu68000::op_ext>},  // EXT.W
        {0xFFF8, 0x48C0, always, &thunk<&Cpu68000::op_ext>},  // EXT.L
        {0xFFF8, 0x4840, always, &thunk<&Cpu68000::op_swap>},
        {0xFFC0, 0x4840, [](uint16_t op) { return ea_ok(op, kEaControl); }, &thunk<&Cpu68000::op_pea>},
        {0xFB80, 0x4880,
         [](uint16_t op) {
             if ((op & 0x0400u) == 0) return ea_ok(op, kEaControlAlterable | ea_bit(4));  // to memory
             return ea_ok(op, kEaControl | ea_bit(3));                                   // to registers
         },
         &thunk<&Cpu68000::op_movem>},
        {0xFFC0, 0x4800, [](uint16_t op) { return ea_ok(op, kEaDataAlterable); }, &thunk<&Cpu68000::op_nbcd>},
        {0xFFC0, 0x40C0, [](uint16_t op) { return ea_ok(op, kEaDataAlterable); },
         &thunk<&Cpu68000::op_move_from_sr>},
        {0xFFC0, 0x44C0, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_move_to_ccr>},
        {0xFFC0, 0x46C0, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_move_to_sr>},
        // NEGX / CLR / NEG / NOT.
        {0xF900, 0x4000,
         [](uint16_t op) { return size_field_valid(op) && ea_ok(op, kEaDataAlterable); },
         &thunk<&Cpu68000::op_neg_not_clr>},
        {0xFFC0, 0x4AC0, [](uint16_t op) { return ea_ok(op, kEaDataAlterable); }, &thunk<&Cpu68000::op_tas>},
        {0xFF00, 0x4A00,
         [](uint16_t op) { return size_field_valid(op) && ea_ok(op, kEaDataAlterable); },
         &thunk<&Cpu68000::op_tst>},
        {0xF1C0, 0x4180, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_chk>},
        {0xF1C0, 0x41C0, [](uint16_t op) { return ea_ok(op, kEaControl); }, &thunk<&Cpu68000::op_lea>},
        // Line 0: bit operations, MOVEP, immediate operations.
        {0xF138, 0x0108, always, &thunk<&Cpu68000::op_movep>},
        {0xF100, 0x0100,
         [](uint16_t op) { return ea_ok(op, ((op >> 6) & 3u) == 0 ? kEaData : kEaDataAlterable); },
         &thunk<&Cpu68000::op_bit_dynamic>},
        {0xFF00, 0x0800,
         [](uint16_t op) {
             return ea_ok(op, ((op >> 6) & 3u) == 0 ? static_cast<uint16_t>(kEaData & ~ea_bit(11))
                                                    : kEaDataAlterable);
         },
         &thunk<&Cpu68000::op_bit_static>},
        {0xF100, 0x0000,
         [](uint16_t op) {
             const unsigned type = (op >> 9) & 7u;  // ORI ANDI SUBI ADDI - EORI CMPI -
             return type != 4 && type != 7 && size_field_valid(op) && ea_ok(op, kEaDataAlterable);
         },
         &thunk<&Cpu68000::op_immediate>},
        // Lines 1-3: MOVE / MOVEA (00ss RRRM MMrr rmmm, ss = 01 B, 11 W, 10 L).
        {0xC000, 0x0000,
         [](uint16_t op) {
             const unsigned size = (op >> 12) & 3u;
             if (size == 0) return false;
             const bool byte = size == 1;
             if (!ea_ok(op, kEaAll) || (byte && ea_mode(op) == 1)) return false;
             const unsigned dst_mode = opmode(op);
             if (dst_mode == 1) return !byte;  // MOVEA
             return ea_in(dst_mode, reg_field(op), kEaDataAlterable);
         },
         &thunk<&Cpu68000::op_move>},
        // Line 5: DBcc, Scc, ADDQ/SUBQ.
        {0xF0F8, 0x50C8, always, &thunk<&Cpu68000::op_dbcc>},
        {0xF0C0, 0x50C0, [](uint16_t op) { return ea_ok(op, kEaDataAlterable); }, &thunk<&Cpu68000::op_scc>},
        {0xF000, 0x5000,
         [](uint16_t op) {
             return size_field_valid(op) && ea_ok(op, kEaAlterable) &&
                    !(((op >> 6) & 3u) == 0 && ea_mode(op) == 1);
         },
         &thunk<&Cpu68000::op_addq_subq>},
        // Lines 6-7: Bcc/BRA/BSR, MOVEQ.
        {0xF000, 0x6000, always, &thunk<&Cpu68000::op_bcc>},
        {0xF100, 0x7000, always, &thunk<&Cpu68000::op_moveq>},
        // Line 8: DIVU/DIVS, SBCD, OR.
        {0xF1C0, 0x80C0, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_div>},
        {0xF1C0, 0x81C0, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_div>},
        {0xF1F0, 0x8100, always, &thunk<&Cpu68000::op_abcd_sbcd>},
        {0xF000, 0x8000, and_or_valid, &thunk<&Cpu68000::op_and_or>},
        // Line 9: SUBX, SUB/SUBA.
        {0xF130, 0x9100, size_field_valid, &thunk<&Cpu68000::op_addx_subx>},
        {0xF000, 0x9000, add_sub_valid, &thunk<&Cpu68000::op_add_sub>},
        // Line B: CMPM, CMPA, CMP, EOR.
        {0xF138, 0xB108, size_field_valid, &thunk<&Cpu68000::op_cmpm>},
        {0xF0C0, 0xB0C0, [](uint16_t op) { return ea_ok(op, kEaAll); }, &thunk<&Cpu68000::op_cmpa>},
        {0xF100, 0xB000,
         [](uint16_t op) { return ea_ok(op, kEaAll) && !(opmode(op) == 0 && ea_mode(op) == 1); },
         &thunk<&Cpu68000::op_cmp>},
        {0xF100, 0xB100,
         [](uint16_t op) { return size_field_valid(op) && ea_ok(op, kEaDataAlterable); },
         &thunk<&Cpu68000::op_eor>},
        // Line C: MULU/MULS, ABCD, EXG, AND.
        {0xF1C0, 0xC0C0, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_mul>},
        {0xF1C0, 0xC1C0, [](uint16_t op) { return ea_ok(op, kEaData); }, &thunk<&Cpu68000::op_mul>},
        {0xF1F0, 0xC100, always, &thunk<&Cpu68000::op_abcd_sbcd>},
        {0xF1F8, 0xC140, always, &thunk<&Cpu68000::op_exg>},
        {0xF1F8, 0xC148, always, &thunk<&Cpu68000::op_exg>},
        {0xF1F8, 0xC188, always, &thunk<&Cpu68000::op_exg>},
        {0xF000, 0xC000, and_or_valid, &thunk<&Cpu68000::op_and_or>},
        // Line D: ADDX, ADD/ADDA.
        {0xF130, 0xD100, size_field_valid, &thunk<&Cpu68000::op_addx_subx>},
        {0xF000, 0xD000, add_sub_valid, &thunk<&Cpu68000::op_add_sub>},
        // Line E: shifts and rotates (memory forms are word-sized, count 1).
        {0xF8C0, 0xE0C0, [](uint16_t op) { return ea_ok(op, kEaMemoryAlterable); },
         &thunk<&Cpu68000::op_shift_memory>},
        {0xF000, 0xE000, size_field_valid, &thunk<&Cpu68000::op_shift_register>},
        // Unassigned lines.
        {0xF000, 0xA000, always, &thunk<&Cpu68000::op_line_a>},
        {0xF000, 0xF000, always, &thunk<&Cpu68000::op_line_f>},
    };

    std::array<Handler, 0x10000> table{};
    for (uint32_t op = 0; op < table.size(); ++op) {
        const auto opcode = static_cast<uint16_t>(op);
        table[op] = &thunk<&Cpu68000::op_illegal>;
        for (const Pattern& p : patterns) {
            if ((opcode & p.mask) == p.match && p.valid(opcode)) {
                table[op] = p.handler;
                break;
            }
        }
    }
    return table;
}

const std::array<Cpu68000::Handler, 0x10000> Cpu68000::dispatch_table_ =
    Cpu68000::build_dispatch_table();

bool Cpu68000::is_implemented(uint16_t opcode) noexcept {
    const Handler handler = dispatch_table_[opcode];
    return handler != &thunk<&Cpu68000::op_illegal> && handler != &thunk<&Cpu68000::op_line_a> &&
           handler != &thunk<&Cpu68000::op_line_f>;
}

// --- Core ---------------------------------------------------------------------

void Cpu68000::reset() {
    sr_ = 0x2700;
    stopped_ = false;
    a_[7] = bus_.read32(0x000000);
    pc_ = bus_.read32(0x000004);
    instruction_pc_ = pc_;
}

void Cpu68000::set_sr(uint16_t value) noexcept {
    value &= kSrImplementedBits;
    if (((sr_ ^ value) & kFlagS) != 0) {
        std::swap(a_[7], other_sp_);
    }
    sr_ = value;
}

uint32_t Cpu68000::step() {
    if (nmi_pending_) {
        nmi_pending_ = false;
        stopped_ = false;
        return take_interrupt(7);
    }
    if (ipl_ > interrupt_mask()) {
        stopped_ = false;
        return take_interrupt(ipl_);
    }
    if (stopped_) return 4;  // STOP: idle until an interrupt

    instruction_pc_ = pc_;
    opcode_ = 0;
    const uint16_t opcode = fetch16();
    opcode_ = opcode;
    return dispatch_table_[opcode](*this, opcode);
}

// Interrupt exception with autovector: enter supervisor mode, clear trace,
// raise the mask to the interrupt level, stack PC and the old SR, then jump
// through vector 24 + level (address $60 + 4 * level).
uint32_t Cpu68000::take_interrupt(uint8_t level) {
    ++exception_counts_[24u + level];
    const uint16_t old_sr = sr_;
    const uint16_t mask_bits = static_cast<uint16_t>(7u << kIntMaskShift);
    set_sr(static_cast<uint16_t>((sr_ | kFlagS) & ~(kFlagT | mask_bits)) |
           static_cast<uint16_t>(level << kIntMaskShift));
    push32(pc_);
    push16(old_sr);
    pc_ = read_memory((24u + level) * 4u, Size::Long);
    return 44;
}

uint32_t Cpu68000::exception(uint8_t vector, uint32_t return_pc, uint32_t cycles) {
    ++exception_counts_[vector];
    const uint16_t old_sr = sr_;
    set_sr(static_cast<uint16_t>((sr_ | kFlagS) & ~kFlagT));
    push32(return_pc);
    push16(old_sr);
    pc_ = read_memory(vector * 4u, Size::Long);
    return cycles;
}

// --- Effective addresses --------------------------------------------------------

Cpu68000::Ea Cpu68000::decode_ea(unsigned mode, unsigned reg, Size size) {
    switch (mode) {
        case 0: return {EaKind::DataReg, reg};
        case 1: return {EaKind::AddrReg, reg};
        case 2: return {EaKind::Memory, a_[reg]};
        case 3: {  // (An)+; byte accesses through A7 keep the stack word-aligned
            const uint32_t address = a_[reg];
            a_[reg] += (size == Size::Byte && reg == 7) ? 2u : static_cast<uint32_t>(size);
            return {EaKind::Memory, address};
        }
        case 4:  // -(An)
            a_[reg] -= (size == Size::Byte && reg == 7) ? 2u : static_cast<uint32_t>(size);
            return {EaKind::Memory, a_[reg]};
        case 5: return {EaKind::Memory, a_[reg] + sign_extend16(fetch16())};
        case 6: return {EaKind::Memory, index_address(a_[reg])};
        default: break;
    }
    switch (reg) {
        case 0: return {EaKind::Memory, sign_extend16(fetch16())};  // (xxx).W
        case 1: return {EaKind::Memory, fetch32()};                 // (xxx).L
        case 2: {  // d16(PC): PC is the address of the extension word
            const uint32_t base = pc_;
            return {EaKind::Memory, base + sign_extend16(fetch16())};
        }
        case 3: return {EaKind::Memory, index_address(pc_)};  // d8(PC,Xn)
        default: return {EaKind::Immediate, immediate(size)};  // #imm (the table rules out reg 5-7)
    }
}

uint32_t Cpu68000::read_ea(const Ea& ea, Size size) {
    const uint32_t mask = size == Size::Byte ? 0xFFu : size == Size::Word ? 0xFFFFu : 0xFFFF'FFFFu;
    switch (ea.kind) {
        case EaKind::DataReg: return d_[ea.value] & mask;
        case EaKind::AddrReg: return a_[ea.value] & mask;
        case EaKind::Memory: return read_memory(ea.value, size);
        case EaKind::Immediate: return ea.value;
    }
    return 0;
}

void Cpu68000::write_ea(const Ea& ea, Size size, uint32_t value) {
    switch (ea.kind) {
        case EaKind::DataReg: write_data_register(ea.value, value, size); break;
        case EaKind::AddrReg: a_[ea.value] = value; break;
        case EaKind::Memory: write_memory(ea.value, size, value); break;
        case EaKind::Immediate: break;  // not alterable: excluded by the dispatch table
    }
}

uint32_t Cpu68000::ea_cycles(unsigned mode, unsigned reg, Size size) noexcept {
    const auto index = static_cast<size_t>(ea_index(mode, reg));
    return size == Size::Long ? kEaCyclesLong[index] : kEaCyclesByteWord[index];
}

// Brief extension word: D/A | reg(3) | W/L | 000 | disp8. The 68000 has no scale factor.
uint32_t Cpu68000::index_address(uint32_t base) {
    const uint16_t ext = fetch16();
    const unsigned index_reg = (ext >> 12) & 7u;
    uint32_t index = (ext & 0x8000u) != 0 ? a_[index_reg] : d_[index_reg];
    if ((ext & 0x0800u) == 0) index = sign_extend16(index);
    return base + index + sign_extend8(ext);
}

// Immediate data: a byte still occupies a whole extension word (low byte).
uint32_t Cpu68000::immediate(Size size) {
    switch (size) {
        case Size::Byte: return fetch16() & 0xFFu;
        case Size::Word: return fetch16();
        case Size::Long: return fetch32();
    }
    return 0;
}

// --- Memory ---------------------------------------------------------------------

uint32_t Cpu68000::read_memory(uint32_t address, Size size) {
    if (size != Size::Byte && (address & 1u) != 0) {
        throw CpuError(CpuError::Kind::OddDataAccess, instruction_pc_, opcode_, address);
    }
    switch (size) {
        case Size::Byte: return bus_.read8(address);
        case Size::Word: return bus_.read16(address);
        case Size::Long: return bus_.read32(address);
    }
    return 0;
}

void Cpu68000::write_memory(uint32_t address, Size size, uint32_t value) {
    if (size != Size::Byte && (address & 1u) != 0) {
        throw CpuError(CpuError::Kind::OddDataAccess, instruction_pc_, opcode_, address);
    }
    switch (size) {
        case Size::Byte: bus_.write8(address, static_cast<uint8_t>(value)); break;
        case Size::Word: bus_.write16(address, static_cast<uint16_t>(value)); break;
        case Size::Long: bus_.write32(address, value); break;
    }
}

uint16_t Cpu68000::fetch16() {
    if ((pc_ & 1u) != 0) {
        throw CpuError(CpuError::Kind::OddPcFetch, pc_, 0);
    }
    const uint16_t word = bus_.read16(pc_);
    pc_ += 2;
    return word;
}

uint32_t Cpu68000::fetch32() {
    const uint32_t hi = fetch16();
    return (hi << 16) | fetch16();
}

void Cpu68000::push16(uint16_t value) {
    a_[7] -= 2;
    write_memory(a_[7], Size::Word, value);
}

void Cpu68000::push32(uint32_t value) {
    a_[7] -= 4;
    write_memory(a_[7], Size::Long, value);
}

uint16_t Cpu68000::pop16() {
    const auto value = static_cast<uint16_t>(read_memory(a_[7], Size::Word));
    a_[7] += 2;
    return value;
}

uint32_t Cpu68000::pop32() {
    const uint32_t value = read_memory(a_[7], Size::Long);
    a_[7] += 4;
    return value;
}

// --- Illegal and unassigned opcodes ------------------------------------------------

uint32_t Cpu68000::op_illegal(uint16_t opcode) {
    if (exception_counts_[kVectorIllegal] < 8) {
        std::fprintf(stderr, "[cpu] illegal instruction $%04X at $%08X\n", opcode, instruction_pc_);
    }
    return exception(kVectorIllegal, instruction_pc_, 34);
}

uint32_t Cpu68000::op_line_a(uint16_t) { return exception(kVectorLineA, instruction_pc_, 34); }
uint32_t Cpu68000::op_line_f(uint16_t) { return exception(kVectorLineF, instruction_pc_, 34); }

// --- Data movement ------------------------------------------------------------------

// MOVE: 00ss RRRM MMrr rmmm. The source is decoded (and its extension words
// fetched) before the destination.
uint32_t Cpu68000::op_move(uint16_t opcode) {
    const unsigned size_bits = (opcode >> 12) & 3u;
    const Size size = size_bits == 1 ? Size::Byte : size_bits == 3 ? Size::Word : Size::Long;
    const unsigned src_mode = ea_mode(opcode);
    const unsigned src_reg = ea_reg(opcode);
    const unsigned dst_mode = opmode(opcode);
    const unsigned dst_reg = reg_field(opcode);

    const uint32_t value = read_ea(decode_ea(src_mode, src_reg, size), size);
    const uint32_t src_cycles = ea_cycles(src_mode, src_reg, size);

    if (dst_mode == 1) {  // MOVEA: whole register, no flags
        a_[dst_reg] = size == Size::Word ? sign_extend16(value) : value;
        return 4 + src_cycles;
    }

    write_ea(decode_ea(dst_mode, dst_reg, size), size, value);
    set_nz_clear_vc(value, size);
    // Destination timing: -(An) costs the same as (An) when writing.
    const uint32_t dst_cycles = dst_mode == 4 ? ea_cycles(2, 0, size) : ea_cycles(dst_mode, dst_reg, size);
    return 4 + src_cycles + dst_cycles;
}

uint32_t Cpu68000::op_moveq(uint16_t opcode) {
    const uint32_t value = sign_extend8(opcode);
    d_[reg_field(opcode)] = value;
    set_nz_clear_vc(value, Size::Long);
    return 4;
}

// MOVEM: the register mask is fetched before the <ea> extension words.
// Mask bit 0 is D0 ... bit 15 is A7, except for -(An) where it is reversed.
// Word transfers to registers are sign-extended to 32 bits.
uint32_t Cpu68000::op_movem(uint16_t opcode) {
    const bool to_registers = (opcode & 0x0400u) != 0;
    const Size size = (opcode & 0x0040u) != 0 ? Size::Long : Size::Word;
    const auto step = static_cast<uint32_t>(size);
    const uint16_t mask = fetch16();
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const auto count = static_cast<uint32_t>(std::popcount(mask));
    const uint32_t per_register = size == Size::Long ? 8 : 4;

    if (!to_registers) {
        if (mode == 4) {  // -(An): stored from A7 down to D0
            const uint32_t original = a_[reg];
            uint32_t address = original;
            for (unsigned bit = 0; bit < 16; ++bit) {
                if ((mask & (1u << bit)) == 0) continue;
                const unsigned r = 15 - bit;  // 0-7 = D0-D7, 8-15 = A0-A7
                address -= step;
                const uint32_t value = r < 8 ? d_[r] : (r - 8 == reg ? original : a_[r - 8]);
                write_memory(address, size, value);
            }
            a_[reg] = address;
            return 8 + per_register * count;
        }
        uint32_t address = decode_ea(mode, reg, size).value;
        for (unsigned r = 0; r < 16; ++r) {
            if ((mask & (1u << r)) == 0) continue;
            write_memory(address, size, r < 8 ? d_[r] : a_[r - 8]);
            address += step;
        }
        // (An) 8, d16(An) 12, d8(An,Xn) 14, abs.W 12, abs.L 16
        static constexpr std::array<uint8_t, 12> kBase = {0, 0, 8, 0, 0, 12, 14, 12, 16, 0, 0, 0};
        return kBase[ea_slot(opcode)] + per_register * count;
    }

    uint32_t address = mode == 3 ? a_[reg] : decode_ea(mode, reg, size).value;
    for (unsigned r = 0; r < 16; ++r) {
        if ((mask & (1u << r)) == 0) continue;
        uint32_t value = read_memory(address, size);
        if (size == Size::Word) value = sign_extend16(value);
        if (r < 8) {
            d_[r] = value;
        } else {
            a_[r - 8] = value;
        }
        address += step;
    }
    if (mode == 3) a_[reg] = address;
    // (An) 12, (An)+ 12, d16(An) 16, d8(An,Xn) 18, abs.W 16, abs.L 20, d16(PC) 16, d8(PC,Xn) 18
    static constexpr std::array<uint8_t, 12> kBase = {0, 0, 12, 12, 0, 16, 18, 16, 20, 16, 18, 0};
    return kBase[ea_slot(opcode)] + per_register * count;
}

// MOVEP: bytes at every other address, d16(Ay) <-> Dx.
uint32_t Cpu68000::op_movep(uint16_t opcode) {
    const unsigned dx = reg_field(opcode);
    const unsigned mode = opmode(opcode) & 3u;  // 0 W mem->reg, 1 L mem->reg, 2 W reg->mem, 3 L reg->mem
    const uint32_t address = a_[ea_reg(opcode)] + sign_extend16(fetch16());
    const unsigned bytes = (mode & 1u) != 0 ? 4 : 2;

    if (mode < 2) {
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value = value << 8 | read_memory(address + 2 * i, Size::Byte);
        write_data_register(dx, value, bytes == 4 ? Size::Long : Size::Word);
    } else {
        for (unsigned i = 0; i < bytes; ++i) {
            write_memory(address + 2 * i, Size::Byte, d_[dx] >> (8 * (bytes - 1 - i)));
        }
    }
    return bytes == 4 ? 24 : 16;
}

uint32_t Cpu68000::op_lea(uint16_t opcode) {
    a_[reg_field(opcode)] = decode_ea(ea_mode(opcode), ea_reg(opcode), Size::Long).value;
    static constexpr std::array<uint8_t, 12> kCycles = {0, 0, 4, 0, 0, 8, 12, 8, 12, 8, 12, 0};
    return kCycles[ea_slot(opcode)];
}

uint32_t Cpu68000::op_pea(uint16_t opcode) {
    push32(decode_ea(ea_mode(opcode), ea_reg(opcode), Size::Long).value);
    static constexpr std::array<uint8_t, 12> kCycles = {0, 0, 12, 0, 0, 16, 20, 16, 20, 16, 20, 0};
    return kCycles[ea_slot(opcode)];
}

uint32_t Cpu68000::op_exg(uint16_t opcode) {
    const unsigned rx = reg_field(opcode);
    const unsigned ry = ea_reg(opcode);
    switch ((opcode >> 3) & 0x1Fu) {
        case 0x08: std::swap(d_[rx], d_[ry]); break;  // Dx,Dy
        case 0x09: std::swap(a_[rx], a_[ry]); break;  // Ax,Ay
        default: std::swap(d_[rx], a_[ry]); break;    // Dx,Ay
    }
    return 6;
}

uint32_t Cpu68000::op_swap(uint16_t opcode) {
    uint32_t& reg = d_[ea_reg(opcode)];
    reg = reg << 16 | reg >> 16;
    set_nz_clear_vc(reg, Size::Long);
    return 4;
}

uint32_t Cpu68000::op_link(uint16_t opcode) {
    const unsigned reg = ea_reg(opcode);
    const uint32_t displacement = sign_extend16(fetch16());
    push32(a_[reg]);
    a_[reg] = a_[7];
    a_[7] += displacement;
    return 16;
}

uint32_t Cpu68000::op_unlk(uint16_t opcode) {
    const unsigned reg = ea_reg(opcode);
    a_[7] = a_[reg];
    a_[reg] = pop32();
    return 12;
}

// --- Arithmetic -------------------------------------------------------------------------

// ADD/SUB: 1101/1001 RRRo oomm mrrr
//   opmode 000/001/010: <ea> op Dn -> Dn     opmode 100/101/110: <ea> op Dn -> <ea>
//   opmode 011/111:     ADDA/SUBA.W/L <ea>,An
uint32_t Cpu68000::op_add_sub(uint16_t opcode) {
    const bool subtract = (opcode >> 12) == 0x9;
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned dn = reg_field(opcode);
    const unsigned om = opmode(opcode);
    const bool reg_or_imm = is_register_or_immediate(mode, reg);

    if (om == 3 || om == 7) {  // ADDA/SUBA: whole register, no flags
        const Size size = om == 3 ? Size::Word : Size::Long;
        uint32_t src = read_ea(decode_ea(mode, reg, size), size);
        if (size == Size::Word) src = sign_extend16(src);
        a_[dn] = subtract ? a_[dn] - src : a_[dn] + src;
        const uint32_t ea = ea_cycles(mode, reg, size);
        return size == Size::Word ? 8 + ea : 6 + ea + (reg_or_imm ? 2 : 0);
    }

    const Size size = (om & 3u) == 0 ? Size::Byte : (om & 3u) == 1 ? Size::Word : Size::Long;
    const uint32_t ea = ea_cycles(mode, reg, size);
    if (om < 3) {  // Dn op <ea> -> Dn
        const uint32_t src = read_ea(decode_ea(mode, reg, size), size);
        const uint32_t result = subtract ? sub_with_flags(src, d_[dn], size) : add_with_flags(src, d_[dn], size);
        write_data_register(dn, result, size);
        // .L: 2 more cycles for register direct / immediate sources (UM table 8-4)
        return size == Size::Long ? 6 + ea + (reg_or_imm ? 2 : 0) : 4 + ea;
    }
    // <ea> op Dn -> <ea> (read-modify-write, address decoded once)
    const Ea dst = decode_ea(mode, reg, size);
    const uint32_t value = read_ea(dst, size);
    write_ea(dst, size, subtract ? sub_with_flags(d_[dn], value, size) : add_with_flags(d_[dn], value, size));
    return (size == Size::Long ? 12 : 8) + ea;
}

uint32_t Cpu68000::op_addq_subq(uint16_t opcode) {
    const bool subtract = (opcode & 0x0100u) != 0;
    uint32_t data = reg_field(opcode);
    if (data == 0) data = 8;
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;

    if (mode == 1) {  // An: whole register, no flags
        a_[reg] = subtract ? a_[reg] - data : a_[reg] + data;
        return 8;
    }
    const Ea ea = decode_ea(mode, reg, size);
    const uint32_t value = read_ea(ea, size);
    write_ea(ea, size, subtract ? sub_with_flags(data, value, size) : add_with_flags(data, value, size));
    if (mode == 0) return size == Size::Long ? 8 : 4;
    return (size == Size::Long ? 12 : 8) + ea_cycles(mode, reg, size);
}

// ADDX/SUBX Dy,Dx or -(Ay),-(Ax).
uint32_t Cpu68000::op_addx_subx(uint16_t opcode) {
    const bool subtract = (opcode >> 12) == 0x9;
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    const unsigned rx = reg_field(opcode);
    const unsigned ry = ea_reg(opcode);

    if ((opcode & 0x0008u) == 0) {
        const uint32_t result = subtract ? sub_with_flags(d_[ry], d_[rx], size, true)
                                         : add_with_flags(d_[ry], d_[rx], size, true);
        write_data_register(rx, result, size);
        return size == Size::Long ? 8 : 4;
    }
    const uint32_t src = read_ea(decode_ea(4, ry, size), size);
    const Ea dst = decode_ea(4, rx, size);
    const uint32_t value = read_ea(dst, size);
    write_ea(dst, size, subtract ? sub_with_flags(src, value, size, true) : add_with_flags(src, value, size, true));
    return size == Size::Long ? 30 : 18;
}

// CMP <ea>,Dn: Dn - <ea>, flags only.
uint32_t Cpu68000::op_cmp(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned om = opmode(opcode);
    const Size size = om == 0 ? Size::Byte : om == 1 ? Size::Word : Size::Long;
    const uint32_t src = read_ea(decode_ea(mode, reg, size), size);
    compare(src, d_[reg_field(opcode)], size);
    return (size == Size::Long ? 6 : 4) + ea_cycles(mode, reg, size);
}

// CMPA <ea>,An: the word form sign-extends the source; always compares 32 bits.
uint32_t Cpu68000::op_cmpa(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const Size size = (opcode & 0x0100u) != 0 ? Size::Long : Size::Word;
    uint32_t src = read_ea(decode_ea(mode, reg, size), size);
    if (size == Size::Word) src = sign_extend16(src);
    compare(src, a_[reg_field(opcode)], Size::Long);
    return 6 + ea_cycles(mode, reg, size);
}

// CMPM (Ay)+,(Ax)+
uint32_t Cpu68000::op_cmpm(uint16_t opcode) {
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    const uint32_t src = read_ea(decode_ea(3, ea_reg(opcode), size), size);
    const uint32_t dst = read_ea(decode_ea(3, reg_field(opcode), size), size);
    compare(src, dst, size);
    return size == Size::Long ? 20 : 12;
}

// ORI/ANDI/SUBI/ADDI/EORI/CMPI #imm,<ea>: the immediate comes before the
// <ea> extension words.
uint32_t Cpu68000::op_immediate(uint16_t opcode) {
    const unsigned type = reg_field(opcode);
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    const uint32_t src = immediate(size);
    const Ea ea = decode_ea(mode, reg, size);
    const uint32_t dst = read_ea(ea, size);
    const bool is_long = size == Size::Long;

    switch (type) {
        case 6:  // CMPI
            compare(src, dst, size);
            if (mode == 0) return is_long ? 14 : 8;
            return (is_long ? 12 : 8) + ea_cycles(mode, reg, size);
        case 0: write_ea(ea, size, dst | src); set_nz_clear_vc(dst | src, size); break;
        case 1: write_ea(ea, size, dst & src); set_nz_clear_vc(dst & src, size); break;
        case 2: write_ea(ea, size, sub_with_flags(src, dst, size)); break;
        case 3: write_ea(ea, size, add_with_flags(src, dst, size)); break;
        default: write_ea(ea, size, dst ^ src); set_nz_clear_vc(dst ^ src, size); break;  // EORI
    }
    if (mode == 0) return is_long ? (type == 1 ? 14 : 16) : 8;
    return (is_long ? 20 : 12) + ea_cycles(mode, reg, size);
}

// NEGX / CLR / NEG / NOT <ea>. (CLR does not perform the 68000's dummy read.)
uint32_t Cpu68000::op_neg_not_clr(uint16_t opcode) {
    const unsigned type = (opcode >> 9) & 3u;  // 0 NEGX, 1 CLR, 2 NEG, 3 NOT
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    const Ea ea = decode_ea(mode, reg, size);

    switch (type) {
        case 0: write_ea(ea, size, sub_with_flags(read_ea(ea, size), 0, size, true)); break;
        case 1: write_ea(ea, size, 0); set_nz_clear_vc(0, size); break;
        case 2: write_ea(ea, size, sub_with_flags(read_ea(ea, size), 0, size)); break;
        default: {
            const uint32_t result = ~read_ea(ea, size);
            write_ea(ea, size, result);
            set_nz_clear_vc(result, size);
            break;
        }
    }
    if (mode == 0) return size == Size::Long ? 6 : 4;
    return (size == Size::Long ? 12 : 8) + ea_cycles(mode, reg, size);
}

uint32_t Cpu68000::op_ext(uint16_t opcode) {
    const unsigned reg = ea_reg(opcode);
    if ((opcode & 0x0040u) != 0) {  // EXT.L: word -> long
        d_[reg] = sign_extend16(d_[reg]);
        set_nz_clear_vc(d_[reg], Size::Long);
    } else {  // EXT.W: byte -> word
        write_data_register(reg, sign_extend8(d_[reg]), Size::Word);
        set_nz_clear_vc(d_[reg], Size::Word);
    }
    return 4;
}

// MULU/MULS <ea>,Dn: 16 x 16 -> 32 bits. Timing depends on the source bits.
uint32_t Cpu68000::op_mul(uint16_t opcode) {
    const bool is_signed = (opcode & 0x0100u) != 0;
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned dn = reg_field(opcode);
    const uint32_t src = read_ea(decode_ea(mode, reg, Size::Word), Size::Word);

    uint32_t result = 0;
    uint32_t bit_cycles = 0;
    if (is_signed) {
        result = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d_[dn])) *
                                       static_cast<int32_t>(static_cast<int16_t>(src)));
        bit_cycles = static_cast<uint32_t>(std::popcount((src << 1 ^ src) & 0xFFFFu));  // 01/10 transitions
    } else {
        result = (d_[dn] & 0xFFFFu) * src;
        bit_cycles = static_cast<uint32_t>(std::popcount(src));
    }
    d_[dn] = result;
    set_nz_clear_vc(result, Size::Long);
    return 38 + 2 * bit_cycles + ea_cycles(mode, reg, Size::Word);
}

// DIVU/DIVS <ea>,Dn: 32 / 16 -> 16-bit quotient (low word), remainder (high
// word). On overflow V is set and Dn is unchanged. Cycles are the worst case.
uint32_t Cpu68000::op_div(uint16_t opcode) {
    const bool is_signed = (opcode & 0x0100u) != 0;
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned dn = reg_field(opcode);
    const uint32_t src = read_ea(decode_ea(mode, reg, Size::Word), Size::Word);
    const uint32_t ea = ea_cycles(mode, reg, Size::Word);

    if (src == 0) return exception(kVectorZeroDivide, pc_, 38 + ea);
    set_flag(kFlagC, false);

    if (!is_signed) {
        const uint32_t quotient = d_[dn] / src;
        const uint32_t remainder = d_[dn] % src;
        if (quotient > 0xFFFF) {
            set_flag(kFlagV, true);
            return 140 + ea;
        }
        d_[dn] = remainder << 16 | quotient;
        set_nz_clear_vc(quotient, Size::Word);
        return 140 + ea;
    }
    const auto dividend = static_cast<int32_t>(d_[dn]);
    const auto divisor = static_cast<int32_t>(static_cast<int16_t>(src));
    if (dividend == INT32_MIN && divisor == -1) {
        set_flag(kFlagV, true);
        return 158 + ea;
    }
    const int32_t quotient = dividend / divisor;  // truncates toward zero, like the 68000
    const int32_t remainder = dividend % divisor;  // takes the dividend's sign
    if (quotient < -32768 || quotient > 32767) {
        set_flag(kFlagV, true);
        return 158 + ea;
    }
    d_[dn] = (static_cast<uint32_t>(remainder) & 0xFFFFu) << 16 | (static_cast<uint32_t>(quotient) & 0xFFFFu);
    set_nz_clear_vc(static_cast<uint32_t>(quotient), Size::Word);
    return 158 + ea;
}

// ABCD/SBCD Dy,Dx or -(Ay),-(Ax): packed BCD with X. Z is only cleared.
uint32_t Cpu68000::op_abcd_sbcd(uint16_t opcode) {
    const bool subtract = (opcode >> 12) == 0x8;
    const unsigned rx = reg_field(opcode);
    const unsigned ry = ea_reg(opcode);
    const bool memory = (opcode & 0x0008u) != 0;

    uint32_t src = 0;
    uint32_t dst = 0;
    Ea dst_ea{EaKind::DataReg, rx};
    if (memory) {
        src = read_ea(decode_ea(4, ry, Size::Byte), Size::Byte);
        dst_ea = decode_ea(4, rx, Size::Byte);
        dst = read_ea(dst_ea, Size::Byte);
    } else {
        src = d_[ry] & 0xFFu;
        dst = d_[rx] & 0xFFu;
    }
    const uint32_t x = flag(kFlagX) ? 1 : 0;
    uint32_t result = 0;
    bool carry = false;
    if (subtract) {
        result = (dst & 0x0Fu) - (src & 0x0Fu) - x;
        if (result > 9) result -= 6;
        result += (dst & 0xF0u) - (src & 0xF0u);
        carry = result > 0x99;
        if (carry) result += 0xA0;
    } else {
        result = (src & 0x0Fu) + (dst & 0x0Fu) + x;
        if (result > 9) result += 6;
        result += (src & 0xF0u) + (dst & 0xF0u);
        carry = result > 0x99;
        if (carry) result -= 0xA0;
    }
    result &= 0xFFu;
    set_flag(kFlagC, carry);
    set_flag(kFlagX, carry);
    set_flag(kFlagN, (result & 0x80u) != 0);  // undefined on the 68000
    set_flag(kFlagV, false);                  // undefined on the 68000
    if (result != 0) set_flag(kFlagZ, false);
    write_ea(dst_ea, Size::Byte, result);
    return memory ? 18 : 6;
}

// NBCD <ea>: 0 - <ea> - X in BCD.
uint32_t Cpu68000::op_nbcd(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const Ea ea = decode_ea(mode, reg, Size::Byte);
    const uint32_t value = read_ea(ea, Size::Byte);
    const uint32_t x = flag(kFlagX) ? 1 : 0;

    uint32_t result = 0u - (value & 0x0Fu) - x;
    if (result > 9) result -= 6;
    result += 0u - (value & 0xF0u);
    const bool borrow = result > 0x99;
    if (borrow) result += 0xA0;
    result &= 0xFFu;
    set_flag(kFlagC, borrow);
    set_flag(kFlagX, borrow);
    set_flag(kFlagN, (result & 0x80u) != 0);
    set_flag(kFlagV, false);
    if (result != 0) set_flag(kFlagZ, false);
    write_ea(ea, Size::Byte, result);
    return mode == 0 ? 6 : 8 + ea_cycles(mode, reg, Size::Byte);
}

// --- Logic ------------------------------------------------------------------------------

// AND/OR: 1100/1000 RRRo oomm mrrr, opmodes as ADD.
uint32_t Cpu68000::op_and_or(uint16_t opcode) {
    const bool is_and = (opcode >> 12) == 0xC;
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned dn = reg_field(opcode);
    const unsigned om = opmode(opcode);
    const Size size = (om & 3u) == 0 ? Size::Byte : (om & 3u) == 1 ? Size::Word : Size::Long;
    const uint32_t ea = ea_cycles(mode, reg, size);

    if (om < 3) {  // <ea> op Dn -> Dn
        const uint32_t src = read_ea(decode_ea(mode, reg, size), size);
        const uint32_t result = is_and ? (d_[dn] & src) : (d_[dn] | src);
        write_data_register(dn, result, size);
        set_nz_clear_vc(result, size);
        return size == Size::Long ? 6 + ea + (is_register_or_immediate(mode, reg) ? 2 : 0) : 4 + ea;
    }
    const Ea dst = decode_ea(mode, reg, size);
    const uint32_t value = read_ea(dst, size);
    const uint32_t result = is_and ? (value & d_[dn]) : (value | d_[dn]);
    write_ea(dst, size, result);
    set_nz_clear_vc(result, size);
    return (size == Size::Long ? 12 : 8) + ea;
}

// EOR Dn,<ea>
uint32_t Cpu68000::op_eor(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    const Ea dst = decode_ea(mode, reg, size);
    const uint32_t result = read_ea(dst, size) ^ d_[reg_field(opcode)];
    write_ea(dst, size, result);
    set_nz_clear_vc(result, size);
    if (mode == 0) return size == Size::Long ? 8 : 4;
    return (size == Size::Long ? 12 : 8) + ea_cycles(mode, reg, size);
}

// TST <ea>: N,Z from the operand; V,C cleared.
uint32_t Cpu68000::op_tst(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    set_nz_clear_vc(read_ea(decode_ea(mode, reg, size), size), size);
    return 4 + ea_cycles(mode, reg, size);
}

// TAS <ea>: test the byte, then set its bit 7 (read-modify-write).
uint32_t Cpu68000::op_tas(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const Ea ea = decode_ea(mode, reg, Size::Byte);
    const uint32_t value = read_ea(ea, Size::Byte);
    set_nz_clear_vc(value, Size::Byte);
    write_ea(ea, Size::Byte, value | 0x80u);
    return mode == 0 ? 4 : 10 + ea_cycles(mode, reg, Size::Byte);
}

// Shifts and rotates. type: 0 AS, 1 LS, 2 ROX, 3 RO. A count of 0 clears C
// (ROX: C = X) and leaves X alone.
namespace {

struct ShiftResult {
    uint32_t value;
    bool carry;
    bool extend;
    bool overflow;
};

ShiftResult shift(unsigned type, bool left, uint32_t value, unsigned count, uint32_t msb, bool x) noexcept {
    const uint32_t mask = msb | (msb - 1);
    value &= mask;
    bool carry = false;
    bool overflow = false;
    for (unsigned i = 0; i < count; ++i) {
        if (left) {
            const bool out = (value & msb) != 0;
            uint32_t next = (value << 1) & mask;
            if (type == 2) next |= x ? 1u : 0u;
            if (type == 3) next |= out ? 1u : 0u;
            if (type == 0 && ((next ^ value) & msb) != 0) overflow = true;
            value = next;
            carry = out;
        } else {
            const bool out = (value & 1u) != 0;
            uint32_t next = value >> 1;
            if (type == 0) next |= value & msb;  // arithmetic: keep the sign
            if (type == 2) next |= x ? msb : 0u;
            if (type == 3) next |= out ? msb : 0u;
            value = next;
            carry = out;
        }
        if (type == 2) x = carry;
    }
    if (count == 0) {
        carry = type == 2 ? x : false;
    } else if (type != 3) {
        x = carry;
    }
    return {value, carry, x, overflow};
}

}  // namespace

uint32_t Cpu68000::op_shift_register(uint16_t opcode) {
    const unsigned count_field = reg_field(opcode);
    const bool left = (opcode & 0x0100u) != 0;
    const unsigned bits = (opcode >> 6) & 3u;
    const Size size = bits == 0 ? Size::Byte : bits == 1 ? Size::Word : Size::Long;
    const unsigned type = (opcode >> 3) & 3u;
    const unsigned reg = ea_reg(opcode);
    const unsigned count =
        (opcode & 0x0020u) != 0 ? d_[count_field] & 63u : (count_field == 0 ? 8u : count_field);
    const uint32_t msb = 1u << (static_cast<unsigned>(size) * 8 - 1);

    const ShiftResult r = shift(type, left, d_[reg], count, msb, flag(kFlagX));
    write_data_register(reg, r.value, size);
    set_nz_clear_vc(r.value, size);
    set_flag(kFlagC, r.carry);
    set_flag(kFlagV, r.overflow);
    set_flag(kFlagX, r.extend);
    return (size == Size::Long ? 8 : 6) + 2 * count;
}

uint32_t Cpu68000::op_shift_memory(uint16_t opcode) {
    const unsigned type = (opcode >> 9) & 3u;
    const bool left = (opcode & 0x0100u) != 0;
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const Ea ea = decode_ea(mode, reg, Size::Word);

    const ShiftResult r = shift(type, left, read_ea(ea, Size::Word), 1, 0x8000u, flag(kFlagX));
    write_ea(ea, Size::Word, r.value);
    set_nz_clear_vc(r.value, Size::Word);
    set_flag(kFlagC, r.carry);
    set_flag(kFlagV, r.overflow);
    set_flag(kFlagX, r.extend);
    return 8 + ea_cycles(mode, reg, Size::Word);
}

uint32_t Cpu68000::op_bit_dynamic(uint16_t opcode) {
    return bit_operation(opcode, d_[reg_field(opcode)], false);
}

uint32_t Cpu68000::op_bit_static(uint16_t opcode) {
    const uint32_t bit_number = fetch16() & 0xFFu;  // fetched before the <ea> extension words
    return bit_operation(opcode, bit_number, true);
}

// BTST/BCHG/BCLR/BSET: Z = !(tested bit), then the bit is left alone,
// inverted, cleared or set. Long on Dn (bit number mod 32), byte on memory
// (bit number mod 8).
uint32_t Cpu68000::bit_operation(uint16_t opcode, uint32_t bit_number, bool immediate_bit_number) {
    const unsigned type = (opcode >> 6) & 3u;  // 0 BTST, 1 BCHG, 2 BCLR, 3 BSET
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const bool on_register = mode == 0;
    const Size size = on_register ? Size::Long : Size::Byte;
    const uint32_t mask = 1u << (bit_number & (on_register ? 31u : 7u));

    const Ea ea = decode_ea(mode, reg, size);
    const uint32_t value = read_ea(ea, size);
    set_flag(kFlagZ, (value & mask) == 0);

    switch (type) {
        case 1: write_ea(ea, size, value ^ mask); break;
        case 2: write_ea(ea, size, value & ~mask); break;
        case 3: write_ea(ea, size, value | mask); break;
        default: break;
    }

    // M68000 UM table 8-9. On Dn, BCHG/BCLR/BSET are 2 cycles faster for bits 0-15.
    const uint32_t extra = immediate_bit_number ? 4 : 0;
    if (!on_register) return (type == 0 ? 4 : 8) + extra + ea_cycles(mode, reg, size);
    if (type == 0) return 6 + extra;
    const uint32_t low_bit_saving = (bit_number & 31u) < 16 ? 2 : 0;
    return (type == 2 ? 10 : 8) + extra - low_bit_saving;
}

uint32_t Cpu68000::op_scc(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const bool set = condition((opcode >> 8) & 0xFu);
    write_ea(decode_ea(mode, reg, Size::Byte), Size::Byte, set ? 0xFFu : 0u);
    if (mode == 0) return set ? 6 : 4;
    return 8 + ea_cycles(mode, reg, Size::Byte);
}

// --- Flow control -------------------------------------------------------------------

// Bcc/BRA/BSR: 0110 cccc dddddddd. An 8-bit displacement of 0 means a 16-bit
// displacement word follows. The base is the address after the opcode word.
uint32_t Cpu68000::op_bcc(uint16_t opcode) {
    const unsigned cc = (opcode >> 8) & 0xFu;
    const uint32_t base = pc_;
    const bool word = (opcode & 0xFFu) == 0;
    const uint32_t displacement = word ? sign_extend16(fetch16()) : sign_extend8(opcode);

    if (cc == 1) {  // BSR: condition slot "false" encodes branch to subroutine
        push32(pc_);
        pc_ = base + displacement;
        return 18;
    }
    if (condition(cc)) {  // cc 0 ("true") is BRA
        pc_ = base + displacement;
        return 10;
    }
    return word ? 12 : 8;
}

// DBcc Dn,label: if cc is false, decrement Dn.W and branch unless it became -1.
uint32_t Cpu68000::op_dbcc(uint16_t opcode) {
    const uint32_t base = pc_;
    const uint32_t displacement = sign_extend16(fetch16());
    if (condition((opcode >> 8) & 0xFu)) return 12;
    const unsigned reg = ea_reg(opcode);
    const uint32_t counter = (d_[reg] - 1) & 0xFFFFu;
    write_data_register(reg, counter, Size::Word);
    if (counter == 0xFFFFu) return 14;
    pc_ = base + displacement;
    return 10;
}

// JMP/JSR: control addressing modes. JSR costs 8 more (the return address push).
uint32_t Cpu68000::op_jmp(uint16_t opcode) {
    pc_ = decode_ea(ea_mode(opcode), ea_reg(opcode), Size::Long).value;
    // (An) 8, d16(An) 10, d8(An,Xn) 14, abs.W 10, abs.L 12, d16(PC) 10, d8(PC,Xn) 14
    static constexpr std::array<uint8_t, 12> kCycles = {0, 0, 8, 0, 0, 10, 14, 10, 12, 10, 14, 0};
    return kCycles[ea_slot(opcode)];
}

uint32_t Cpu68000::op_jsr(uint16_t opcode) {
    const uint32_t target = decode_ea(ea_mode(opcode), ea_reg(opcode), Size::Long).value;
    push32(pc_);  // address after the extension words
    pc_ = target;
    static constexpr std::array<uint8_t, 12> kCycles = {0, 0, 16, 0, 0, 18, 22, 18, 20, 18, 22, 0};
    return kCycles[ea_slot(opcode)];
}

uint32_t Cpu68000::op_rts(uint16_t) {
    pc_ = pop32();
    return 16;
}

uint32_t Cpu68000::op_rtr(uint16_t) {
    const uint16_t ccr = pop16();
    sr_ = static_cast<uint16_t>((sr_ & 0xFF00u) | (ccr & 0x1Fu));
    pc_ = pop32();
    return 20;
}

// RTE: pop SR, then PC, from the supervisor stack. Privileged.
uint32_t Cpu68000::op_rte(uint16_t) {
    if (!supervisor()) return privilege_violation();
    const uint16_t new_sr = pop16();
    const uint32_t new_pc = pop32();
    set_sr(new_sr);  // may switch back to the user stack
    pc_ = new_pc;
    return 20;
}

uint32_t Cpu68000::op_trap(uint16_t opcode) {
    return exception(static_cast<uint8_t>(kVectorTrap0 + (opcode & 0xFu)), pc_, 34);
}

uint32_t Cpu68000::op_trapv(uint16_t) {
    return flag(kFlagV) ? exception(kVectorTrapV, pc_, 34) : 4;
}

// CHK <ea>,Dn: trap if Dn.W < 0 (N set) or Dn.W > <ea> (N cleared).
uint32_t Cpu68000::op_chk(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const auto bound = static_cast<int16_t>(read_ea(decode_ea(mode, reg, Size::Word), Size::Word));
    const auto value = static_cast<int16_t>(d_[reg_field(opcode)]);
    const uint32_t ea = ea_cycles(mode, reg, Size::Word);
    if (value < 0) {
        set_flag(kFlagN, true);
        return exception(kVectorChk, pc_, 40 + ea);
    }
    if (value > bound) {
        set_flag(kFlagN, false);
        return exception(kVectorChk, pc_, 40 + ea);
    }
    return 10 + ea;
}

uint32_t Cpu68000::op_nop(uint16_t) { return 4; }

// --- System control ---------------------------------------------------------------------

uint32_t Cpu68000::op_move_from_sr(uint16_t opcode) {  // not privileged on the 68000
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    write_ea(decode_ea(mode, reg, Size::Word), Size::Word, sr_);
    return mode == 0 ? 6 : 8 + ea_cycles(mode, reg, Size::Word);
}

uint32_t Cpu68000::op_move_to_ccr(uint16_t opcode) {
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    const uint32_t value = read_ea(decode_ea(mode, reg, Size::Word), Size::Word);
    sr_ = static_cast<uint16_t>((sr_ & 0xFF00u) | (value & 0x1Fu));
    return 12 + ea_cycles(mode, reg, Size::Word);
}

uint32_t Cpu68000::op_move_to_sr(uint16_t opcode) {
    if (!supervisor()) return privilege_violation();
    const unsigned mode = ea_mode(opcode);
    const unsigned reg = ea_reg(opcode);
    set_sr(static_cast<uint16_t>(read_ea(decode_ea(mode, reg, Size::Word), Size::Word)));
    return 12 + ea_cycles(mode, reg, Size::Word);
}

// MOVE USP: bit 3 set = USP -> An, clear = An -> USP. Privileged.
uint32_t Cpu68000::op_move_usp(uint16_t opcode) {
    if (!supervisor()) return privilege_violation();
    const unsigned reg = ea_reg(opcode);
    if ((opcode & 0x0008u) != 0) {
        a_[reg] = other_sp_;
    } else {
        other_sp_ = a_[reg];
    }
    return 4;
}

// ORI/ANDI/EORI #imm to CCR (byte) or SR (word, privileged).
uint32_t Cpu68000::op_logic_to_ccr_sr(uint16_t opcode) {
    const bool to_sr = (opcode & 0x0040u) != 0;
    const unsigned type = reg_field(opcode);  // 0 OR, 1 AND, 5 EOR
    const uint16_t operand = fetch16();
    if (to_sr && !supervisor()) return privilege_violation();

    const auto apply = [type](uint16_t value, uint16_t imm) -> uint16_t {
        switch (type) {
            case 0: return value | imm;
            case 1: return value & imm;
            default: return value ^ imm;
        }
    };
    if (to_sr) {
        set_sr(apply(sr_, operand));
    } else {
        const uint16_t ccr = apply(sr_ & 0xFFu, operand & 0xFFu);
        sr_ = static_cast<uint16_t>((sr_ & 0xFF00u) | (ccr & 0x1Fu));
    }
    return 20;
}

// STOP #imm: load SR and wait for an interrupt. Privileged.
uint32_t Cpu68000::op_stop(uint16_t) {
    const uint16_t value = fetch16();
    if (!supervisor()) return privilege_violation();
    set_sr(value);
    stopped_ = true;
    return 4;
}

// RESET: pulses the reset line of the other chips, not the CPU. Privileged.
uint32_t Cpu68000::op_reset(uint16_t) {
    if (!supervisor()) return privilege_violation();
    if (reset_handler_ != nullptr) reset_handler_(reset_context_);
    return 132;
}

// --- Registers and flags ------------------------------------------------------

// Byte and word writes leave the upper part of Dn untouched.
void Cpu68000::write_data_register(unsigned reg, uint32_t value, Size size) noexcept {
    switch (size) {
        case Size::Byte: d_[reg] = (d_[reg] & 0xFFFF'FF00u) | (value & 0xFFu); break;
        case Size::Word: d_[reg] = (d_[reg] & 0xFFFF'0000u) | (value & 0xFFFFu); break;
        case Size::Long: d_[reg] = value; break;
    }
}

void Cpu68000::set_nz_clear_vc(uint32_t result, Size size) noexcept {
    const uint32_t msb = 1u << (static_cast<unsigned>(size) * 8 - 1);
    const uint32_t mask = msb | (msb - 1);
    uint16_t sr = sr_ & static_cast<uint16_t>(~(kFlagN | kFlagZ | kFlagV | kFlagC));
    if ((result & mask) == 0) sr |= kFlagZ;
    if ((result & msb) != 0) sr |= kFlagN;
    sr_ = sr;
}

// dst + src (+ X for ADDX, where Z is only ever cleared).
uint32_t Cpu68000::add_with_flags(uint32_t src, uint32_t dst, Size size, bool extend) noexcept {
    const uint32_t msb = 1u << (static_cast<unsigned>(size) * 8 - 1);
    const uint32_t mask = msb | (msb - 1);
    src &= mask;
    dst &= mask;
    const uint64_t sum = uint64_t{src} + dst + (extend && flag(kFlagX) ? 1u : 0u);
    const uint32_t result = static_cast<uint32_t>(sum) & mask;

    const bool keep_z = extend && flag(kFlagZ);
    uint16_t sr = sr_ & static_cast<uint16_t>(~(kFlagX | kFlagN | kFlagZ | kFlagV | kFlagC));
    if (result == 0 && (!extend || keep_z)) sr |= kFlagZ;
    if ((result & msb) != 0) sr |= kFlagN;
    if (((src ^ result) & (dst ^ result) & msb) != 0) sr |= kFlagV;  // operands same sign, result differs
    if (sum > mask) sr |= kFlagC | kFlagX;
    sr_ = sr;
    return result;
}

// dst - src (- X for SUBX/NEGX, where Z is only ever cleared).
uint32_t Cpu68000::sub_with_flags(uint32_t src, uint32_t dst, Size size, bool extend) noexcept {
    const uint32_t msb = 1u << (static_cast<unsigned>(size) * 8 - 1);
    const uint32_t mask = msb | (msb - 1);
    src &= mask;
    dst &= mask;
    const uint64_t subtrahend = uint64_t{src} + (extend && flag(kFlagX) ? 1u : 0u);
    const uint32_t result = static_cast<uint32_t>(dst - subtrahend) & mask;

    const bool keep_z = extend && flag(kFlagZ);
    uint16_t sr = sr_ & static_cast<uint16_t>(~(kFlagX | kFlagN | kFlagZ | kFlagV | kFlagC));
    if (result == 0 && (!extend || keep_z)) sr |= kFlagZ;
    if ((result & msb) != 0) sr |= kFlagN;
    if (((src ^ dst) & (result ^ dst) & msb) != 0) sr |= kFlagV;  // operands differ in sign, result sign != dst
    if (subtrahend > dst) sr |= kFlagC | kFlagX;  // borrow
    sr_ = sr;
    return result;
}

// dst - src: N,Z,V,C like SUB; X unaffected.
void Cpu68000::compare(uint32_t src, uint32_t dst, Size size) noexcept {
    const bool x = flag(kFlagX);
    sub_with_flags(src, dst, size);
    set_flag(kFlagX, x);
}

bool Cpu68000::condition(unsigned cc) const noexcept {
    const bool c = (sr_ & kFlagC) != 0;
    const bool v = (sr_ & kFlagV) != 0;
    const bool z = (sr_ & kFlagZ) != 0;
    const bool n = (sr_ & kFlagN) != 0;
    switch (cc & 0xFu) {
        case 0x0: return true;          // T
        case 0x1: return false;         // F
        case 0x2: return !c && !z;      // HI
        case 0x3: return c || z;        // LS
        case 0x4: return !c;            // CC (HS)
        case 0x5: return c;             // CS (LO)
        case 0x6: return !z;            // NE
        case 0x7: return z;             // EQ
        case 0x8: return !v;            // VC
        case 0x9: return v;             // VS
        case 0xA: return !n;            // PL
        case 0xB: return n;             // MI
        case 0xC: return n == v;        // GE
        case 0xD: return n != v;        // LT
        case 0xE: return !z && n == v;  // GT
        default: return z || n != v;    // LE
    }
}

}  // namespace amiga
