#pragma once

#include <array>
#include <cstdint>
#include <exception>

#include "core/memory_bus.hpp"

namespace amiga {

// Raised when the CPU hits something the emulator can't execute. Like
// BusError, this is a panic: execution cannot meaningfully continue.
class CpuError final : public std::exception {
public:
    enum class Kind : uint8_t {
        UnimplementedOpcode,  // kept for completeness; all 68000 opcodes are decoded
        DoubleBusFault,       // address error while processing an address error: the 68000 halts
    };

    CpuError(Kind kind, uint32_t pc, uint16_t opcode, uint32_t address = 0) noexcept;

    [[nodiscard]] const char* what() const noexcept override { return message_.data(); }
    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] uint32_t pc() const noexcept { return pc_; }
    [[nodiscard]] uint16_t opcode() const noexcept { return opcode_; }
    [[nodiscard]] uint32_t address() const noexcept { return address_; }

private:
    Kind kind_;
    uint32_t pc_;
    uint16_t opcode_;
    uint32_t address_;
    std::array<char, 96> message_{};
};

// Motorola 68000 interpreter (M68000 Family Programmer's Reference Manual,
// M68000 User's Manual for timings).
//
// Decoding goes through a 65536-entry table of handlers, one per opcode word,
// built once at startup. Every 68000 instruction is implemented; the opcode
// words left over raise the illegal instruction exception (or line A / line F).
// Operands go through a generic effective address engine covering all 12
// addressing modes. Instructions return their cycle count from the manual's
// timing tables (DIVU/DIVS use their worst case).
//
// Exceptions: interrupts (autovectored, as on the Amiga; level 7 is
// edge-triggered), address error (a word/long access or instruction fetch at
// an odd address: group 0, 14-byte frame), illegal instruction, line A/F,
// privilege violation, TRAP, TRAPV, CHK and divide by zero. STOP waits for an
// interrupt. An address error while stacking an address error frame is a
// double bus fault: the 68000 halts, reported with CpuError.
//
// Not modelled: the prefetch queue, exact bus cycle order and trace mode.
// The PC stacked by an address error is the faulting instruction + 2 (the
// real 68000 stacks a value 2-10 bytes past it, depending on the instruction).
class Cpu68000 {
public:
    // Status register bits.
    static constexpr uint16_t kFlagC = 1u << 0;
    static constexpr uint16_t kFlagV = 1u << 1;
    static constexpr uint16_t kFlagZ = 1u << 2;
    static constexpr uint16_t kFlagN = 1u << 3;
    static constexpr uint16_t kFlagX = 1u << 4;
    static constexpr uint16_t kIntMaskShift = 8;  // I2-I0 in bits 10-8
    static constexpr uint16_t kFlagS = 1u << 13;
    static constexpr uint16_t kFlagT = 1u << 15;
    static constexpr uint16_t kSrImplementedBits = 0xA71F;  // T . S . . I2 I1 I0 . . . X N Z V C

    // Exception vector numbers.
    static constexpr uint8_t kVectorAddressError = 3;
    static constexpr uint64_t kMaxAddressErrorLogs = 16;  // then counted silently
    static constexpr uint8_t kVectorIllegal = 4;
    static constexpr uint8_t kVectorZeroDivide = 5;
    static constexpr uint8_t kVectorChk = 6;
    static constexpr uint8_t kVectorTrapV = 7;
    static constexpr uint8_t kVectorPrivilege = 8;
    static constexpr uint8_t kVectorLineA = 10;
    static constexpr uint8_t kVectorLineF = 11;
    static constexpr uint8_t kVectorTrap0 = 32;

    // Called when the RESET instruction pulses the reset line (not the CPU itself).
    using ResetHandler = void (*)(void* context);

    explicit Cpu68000(MemoryBus& bus) noexcept : bus_(bus) {}

    // Reset exception: supervisor mode, interrupts masked (SR=$2700),
    // SSP from $000000 and PC from $000004.
    void reset();

    // Executes one instruction, takes a pending interrupt, or idles while
    // stopped, and returns the number of CPU clock cycles it takes.
    uint32_t step();

    // State of the IPL0-IPL2 inputs (0 = no interrupt, 1-7 = level).
    void set_interrupt_level(uint8_t level) noexcept {
        if (level == 7 && ipl_ != 7) nmi_pending_ = true;
        ipl_ = level & 7u;
    }
    // Same as set_interrupt_level: the level stays asserted (the IPL lines are
    // level-sensitive) until changed; 0 releases it. Out-of-range values are clamped.
    void assert_interrupt(int level) noexcept {
        set_interrupt_level(static_cast<uint8_t>(level < 0 ? 0 : level > 7 ? 7 : level));
    }
    [[nodiscard]] uint8_t interrupt_mask() const noexcept {
        return static_cast<uint8_t>((sr_ >> kIntMaskShift) & 7u);
    }

    void set_reset_handler(ResetHandler handler, void* context) noexcept {
        reset_handler_ = handler;
        reset_context_ = context;
    }

    [[nodiscard]] uint32_t d(unsigned n) const noexcept { return d_[n & 7]; }
    [[nodiscard]] uint32_t a(unsigned n) const noexcept { return a_[n & 7]; }
    [[nodiscard]] uint32_t pc() const noexcept { return pc_; }
    [[nodiscard]] uint16_t sr() const noexcept { return sr_; }
    [[nodiscard]] uint32_t usp() const noexcept { return supervisor() ? other_sp_ : a_[7]; }
    [[nodiscard]] uint32_t ssp() const noexcept { return supervisor() ? a_[7] : other_sp_; }
    [[nodiscard]] bool supervisor() const noexcept { return (sr_ & kFlagS) != 0; }
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] uint64_t exception_count(uint8_t vector) const noexcept { return exception_counts_[vector]; }

    void set_d(unsigned n, uint32_t value) noexcept { d_[n & 7] = value; }
    void set_a(unsigned n, uint32_t value) noexcept { a_[n & 7] = value; }
    void set_pc(uint32_t value) noexcept { pc_ = value; }
    // Switching the S bit swaps the active stack pointer (A7) between USP and SSP.
    void set_sr(uint16_t value) noexcept;

    // True if the opcode word decodes to an instruction (not an illegal/line A/F trap).
    [[nodiscard]] static bool is_implemented(uint16_t opcode) noexcept;

private:
    enum class Size : uint8_t { Byte = 1, Word = 2, Long = 4 };

    // A decoded effective address. Side effects ((An)+, -(An)) and extension
    // word fetches happen when it is decoded, exactly once per operand.
    enum class EaKind : uint8_t { DataReg, AddrReg, Memory, Immediate };
    struct Ea {
        EaKind kind;
        uint32_t value;  // register number, memory address or immediate data
    };

    using Handler = uint32_t (*)(Cpu68000&, uint16_t);
    static std::array<Handler, 0x10000> build_dispatch_table();
    static const std::array<Handler, 0x10000> dispatch_table_;

    template <uint32_t (Cpu68000::*Op)(uint16_t)>
    static uint32_t thunk(Cpu68000& cpu, uint16_t opcode) {
        return (cpu.*Op)(opcode);
    }

    // --- Instruction handlers (cpu68000.cpp) ---
    uint32_t op_illegal(uint16_t opcode);
    uint32_t op_line_a(uint16_t opcode);
    uint32_t op_line_f(uint16_t opcode);
    // Data movement
    uint32_t op_move(uint16_t opcode);
    uint32_t op_moveq(uint16_t opcode);
    uint32_t op_movem(uint16_t opcode);
    uint32_t op_movep(uint16_t opcode);
    uint32_t op_lea(uint16_t opcode);
    uint32_t op_pea(uint16_t opcode);
    uint32_t op_exg(uint16_t opcode);
    uint32_t op_swap(uint16_t opcode);
    uint32_t op_link(uint16_t opcode);
    uint32_t op_unlk(uint16_t opcode);
    // Arithmetic
    uint32_t op_add_sub(uint16_t opcode);
    uint32_t op_addq_subq(uint16_t opcode);
    uint32_t op_addx_subx(uint16_t opcode);
    uint32_t op_cmp(uint16_t opcode);
    uint32_t op_cmpa(uint16_t opcode);
    uint32_t op_cmpm(uint16_t opcode);
    uint32_t op_immediate(uint16_t opcode);  // ORI ANDI SUBI ADDI EORI CMPI
    uint32_t op_neg_not_clr(uint16_t opcode);  // NEGX CLR NEG NOT
    uint32_t op_ext(uint16_t opcode);
    uint32_t op_mul(uint16_t opcode);
    uint32_t op_div(uint16_t opcode);
    uint32_t op_abcd_sbcd(uint16_t opcode);
    uint32_t op_nbcd(uint16_t opcode);
    // Logic
    uint32_t op_and_or(uint16_t opcode);
    uint32_t op_eor(uint16_t opcode);
    uint32_t op_tst(uint16_t opcode);
    uint32_t op_tas(uint16_t opcode);
    uint32_t op_shift_register(uint16_t opcode);
    uint32_t op_shift_memory(uint16_t opcode);
    uint32_t op_bit_dynamic(uint16_t opcode);
    uint32_t op_bit_static(uint16_t opcode);
    uint32_t op_scc(uint16_t opcode);
    // Flow control
    uint32_t op_bcc(uint16_t opcode);
    uint32_t op_dbcc(uint16_t opcode);
    uint32_t op_jmp(uint16_t opcode);
    uint32_t op_jsr(uint16_t opcode);
    uint32_t op_rts(uint16_t opcode);
    uint32_t op_rtr(uint16_t opcode);
    uint32_t op_rte(uint16_t opcode);
    uint32_t op_trap(uint16_t opcode);
    uint32_t op_trapv(uint16_t opcode);
    uint32_t op_chk(uint16_t opcode);
    uint32_t op_nop(uint16_t opcode);
    // System control
    uint32_t op_move_from_sr(uint16_t opcode);
    uint32_t op_move_to_ccr(uint16_t opcode);
    uint32_t op_move_to_sr(uint16_t opcode);
    uint32_t op_move_usp(uint16_t opcode);
    uint32_t op_logic_to_ccr_sr(uint16_t opcode);  // ORI/ANDI/EORI to CCR/SR
    uint32_t op_stop(uint16_t opcode);
    uint32_t op_reset(uint16_t opcode);

    uint32_t bit_operation(uint16_t opcode, uint32_t bit_number, bool immediate_bit_number);
    uint32_t take_interrupt(uint8_t level);
    // Group 1/2 exception: stack return PC and SR, enter supervisor mode, jump.
    uint32_t exception(uint8_t vector, uint32_t return_pc, uint32_t cycles);
    uint32_t privilege_violation() { return exception(kVectorPrivilege, instruction_pc_, 34); }

    // Raised by memory accesses and fetches at odd addresses; turned into the
    // address error exception by step().
    struct AddressFault {
        uint32_t address;
        bool read;
        bool instruction;  // program space (fetch) rather than data
    };
    uint32_t address_error(const AddressFault& fault);
    uint32_t execute();

    // Effective address engine.
    Ea decode_ea(unsigned mode, unsigned reg, Size size);
    uint32_t read_ea(const Ea& ea, Size size);
    void write_ea(const Ea& ea, Size size, uint32_t value);
    [[nodiscard]] static uint32_t ea_cycles(unsigned mode, unsigned reg, Size size) noexcept;
    uint32_t index_address(uint32_t base);  // d8(base,Xn) brief extension word
    uint32_t immediate(Size size);

    // Memory access with 68000 alignment rules.
    uint32_t read_memory(uint32_t address, Size size);
    void write_memory(uint32_t address, Size size, uint32_t value);
    uint16_t fetch16();
    uint32_t fetch32();
    void push16(uint16_t value);
    void push32(uint32_t value);
    uint16_t pop16();
    uint32_t pop32();

    // Flags.
    void write_data_register(unsigned reg, uint32_t value, Size size) noexcept;
    void set_nz_clear_vc(uint32_t result, Size size) noexcept;
    uint32_t add_with_flags(uint32_t src, uint32_t dst, Size size, bool extend = false) noexcept;
    uint32_t sub_with_flags(uint32_t src, uint32_t dst, Size size, bool extend = false) noexcept;
    void compare(uint32_t src, uint32_t dst, Size size) noexcept;
    [[nodiscard]] bool condition(unsigned cc) const noexcept;
    void set_flag(uint16_t flag, bool value) noexcept {
        sr_ = value ? static_cast<uint16_t>(sr_ | flag) : static_cast<uint16_t>(sr_ & ~flag);
    }
    [[nodiscard]] bool flag(uint16_t f) const noexcept { return (sr_ & f) != 0; }

    MemoryBus& bus_;
    std::array<uint32_t, 8> d_{};
    std::array<uint32_t, 8> a_{};  // a_[7] is the active stack pointer
    uint32_t other_sp_ = 0;        // the inactive one (USP in supervisor mode, SSP in user mode)
    uint32_t pc_ = 0;
    uint16_t sr_ = 0x2700;
    uint32_t instruction_pc_ = 0;  // address of the instruction being executed
    uint16_t opcode_ = 0;          // opcode being executed (for error reports)
    uint8_t ipl_ = 0;
    bool nmi_pending_ = false;
    bool stopped_ = false;
    bool in_address_error_ = false;
    ResetHandler reset_handler_ = nullptr;
    void* reset_context_ = nullptr;
    std::array<uint64_t, 256> exception_counts_{};
};

}  // namespace amiga
