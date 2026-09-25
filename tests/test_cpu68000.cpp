#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "core/cpu68000.hpp"
#include "core/memory_bus.hpp"
#include "test_framework.hpp"

namespace {

using amiga::Cpu68000;
using amiga::CpuError;
using amiga::MemoryBus;

constexpr uint32_t kProgramBase = 0x1000;
constexpr uint16_t kNZVC = Cpu68000::kFlagN | Cpu68000::kFlagZ | Cpu68000::kFlagV | Cpu68000::kFlagC;

// Chip RAM with a program at kProgramBase and a CPU pointing at it.
struct Fixture {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    Cpu68000 cpu{*bus};

    explicit Fixture(std::initializer_list<uint16_t> program) {
        bus->set_overlay(false);
        uint32_t address = kProgramBase;
        for (const uint16_t word : program) {
            bus->write16(address, word);
            address += 2;
        }
        cpu.set_pc(kProgramBase);
        cpu.set_a(7, 0x8000);            // supervisor stack for exceptions
        bus->write32(4 * 4, 0x4000);      // illegal instruction vector
    }

    // Executes one instruction and reports whether it took the illegal instruction exception.
    bool takes_illegal_exception() {
        cpu.step();
        return cpu.pc() == 0x4000 && bus->read32(0x8000 - 4) == kProgramBase;
    }

    [[nodiscard]] uint16_t flags() const { return cpu.sr() & (kNZVC | Cpu68000::kFlagX); }
};

// --- MOVE ---------------------------------------------------------------------

void test_move_long_data_to_data() {
    Fixture f{0x2401};  // MOVE.L D1,D2
    f.cpu.set_d(1, 0x8000'0001);
    f.cpu.set_sr(0x2700 | Cpu68000::kFlagX | Cpu68000::kFlagV | Cpu68000::kFlagC);
    EXPECT(f.cpu.step() == 4);
    EXPECT(f.cpu.d(2) == 0x8000'0001);
    EXPECT(f.cpu.pc() == kProgramBase + 2);
    EXPECT(f.flags() == (Cpu68000::kFlagN | Cpu68000::kFlagX));  // V,C cleared; X kept
}

void test_move_byte_preserves_upper_bits_and_sets_z() {
    Fixture f{0x1200};  // MOVE.B D0,D1
    f.cpu.set_d(0, 0x1234'5600);
    f.cpu.set_d(1, 0xAABB'CCDD);
    f.cpu.step();
    EXPECT(f.cpu.d(1) == 0xAABB'CC00);
    EXPECT(f.flags() == Cpu68000::kFlagZ);
}

void test_move_word_from_address_register() {
    Fixture f{0x380B};  // MOVE.W A3,D4
    f.cpu.set_a(3, 0x0001'8000);
    f.cpu.set_d(4, 0xFFFF'FFFF);
    f.cpu.step();
    EXPECT(f.cpu.d(4) == 0xFFFF'8000);
    EXPECT(f.flags() == Cpu68000::kFlagN);
}

void test_movea_word_sign_extends_without_flags() {
    Fixture f{0x3240};  // MOVEA.W D0,A1
    f.cpu.set_d(0, 0x0000'FFFE);
    f.cpu.set_sr(0x2700 | Cpu68000::kFlagZ);
    EXPECT(f.cpu.step() == 4);
    EXPECT(f.cpu.a(1) == 0xFFFF'FFFE);
    EXPECT(f.flags() == Cpu68000::kFlagZ);  // unchanged
}

void test_move_illegal_forms_trap() {
    Fixture byte_from_an{0x1008};  // MOVE.B A0,D0: byte access to An is illegal
    EXPECT(byte_from_an.takes_illegal_exception());

    Fixture pc_relative_dest{0x25C0, 0x0010};  // MOVE.L D0,16(PC): destination not alterable
    EXPECT(pc_relative_dest.takes_illegal_exception());

    Fixture movea_byte{0x1040};  // MOVEA.B does not exist
    EXPECT(movea_byte.takes_illegal_exception());
}

// --- ADD ----------------------------------------------------------------------

void test_add_long_overflow() {
    Fixture f{0xD081};  // ADD.L D1,D0
    f.cpu.set_d(0, 0x7FFF'FFFF);
    f.cpu.set_d(1, 1);
    EXPECT(f.cpu.step() == 8);
    EXPECT(f.cpu.d(0) == 0x8000'0000);
    EXPECT(f.flags() == (Cpu68000::kFlagN | Cpu68000::kFlagV));
}

void test_add_byte_carry_to_zero() {
    Fixture f{0xD001};  // ADD.B D1,D0
    f.cpu.set_d(0, 0x1234'56FF);
    f.cpu.set_d(1, 0x0000'0001);
    EXPECT(f.cpu.step() == 4);
    EXPECT(f.cpu.d(0) == 0x1234'5600);
    EXPECT(f.flags() == (Cpu68000::kFlagZ | Cpu68000::kFlagC | Cpu68000::kFlagX));
}

void test_add_word_negative_plus_negative() {
    Fixture f{0xD049};  // ADD.W A1,D0
    f.cpu.set_d(0, 0xABCD'8000);
    f.cpu.set_a(1, 0x0000'8000);
    f.cpu.step();
    EXPECT(f.cpu.d(0) == 0xABCD'0000);
    EXPECT(f.flags() == (Cpu68000::kFlagZ | Cpu68000::kFlagV | Cpu68000::kFlagC | Cpu68000::kFlagX));
}

void test_add_clears_stale_flags() {
    Fixture f{0xD081};  // ADD.L D1,D0
    f.cpu.set_d(0, 2);
    f.cpu.set_d(1, 3);
    f.cpu.set_sr(0x2700 | kNZVC | Cpu68000::kFlagX);
    f.cpu.step();
    EXPECT(f.cpu.d(0) == 5);
    EXPECT(f.flags() == 0);
}

void test_adda_word_sign_extends_without_flags() {
    Fixture f{0xD0C1};  // ADDA.W D1,A0
    f.cpu.set_a(0, 0x0000'1000);
    f.cpu.set_d(1, 0x0000'FFFF);  // -1 as a word
    EXPECT(f.cpu.step() == 8);
    EXPECT(f.cpu.a(0) == 0x0000'0FFF);
    EXPECT(f.flags() == 0);
}

void test_addx_encoding_is_not_add() {
    Fixture f{0xD181};  // ADDX.L D1,D0: adds X too, Z only cleared
    f.cpu.set_d(0, 1);
    f.cpu.set_d(1, 2);
    f.cpu.set_sr(0x2700 | Cpu68000::kFlagX | Cpu68000::kFlagZ);
    EXPECT(f.cpu.step() == 8);
    EXPECT(f.cpu.d(0) == 4);
    EXPECT(f.flags() == 0);
}

// --- JMP ----------------------------------------------------------------------

void test_jmp_address_register_indirect() {
    Fixture f{0x4ED0};  // JMP (A0)
    f.cpu.set_a(0, 0x00FC'00D2);
    EXPECT(f.cpu.step() == 8);
    EXPECT(f.cpu.pc() == 0x00FC'00D2);
}

void test_jmp_displacement() {
    Fixture f{0x4EE8, 0xFFF0};  // JMP -16(A0)
    f.cpu.set_a(0, 0x2000);
    EXPECT(f.cpu.step() == 10);
    EXPECT(f.cpu.pc() == 0x1FF0);
}

void test_jmp_absolute() {
    Fixture f{0x4EF9, 0x00FC, 0x00D2};  // JMP $00FC00D2.L
    EXPECT(f.cpu.step() == 12);
    EXPECT(f.cpu.pc() == 0x00FC'00D2);

    Fixture w{0x4EF8, 0x8000};  // JMP $8000.W -> sign-extended
    EXPECT(w.cpu.step() == 10);
    EXPECT(w.cpu.pc() == 0xFFFF'8000);
}

void test_jmp_pc_relative() {
    Fixture f{0x4EFA, 0x0010};  // JMP 16(PC): base is the extension word address
    EXPECT(f.cpu.step() == 10);
    EXPECT(f.cpu.pc() == kProgramBase + 2 + 0x10);

    Fixture x{0x4EFB, 0x1004};  // JMP 4(PC,D1.W)
    x.cpu.set_d(1, 0x0001'FFFE);  // .W index -> -2
    EXPECT(x.cpu.step() == 14);
    EXPECT(x.cpu.pc() == kProgramBase + 2 + 4 - 2);
}

void test_jmp_indexed() {
    Fixture f{0x4EF0, 0x98FE};  // JMP -2(A0,A1.L)
    f.cpu.set_a(0, 0x3000);
    f.cpu.set_a(1, 0x0001'0000);
    EXPECT(f.cpu.step() == 14);
    EXPECT(f.cpu.pc() == 0x0001'2FFE);
}

void test_jmp_illegal_modes_trap() {
    Fixture f{0x4EC0};  // JMP D0 is illegal
    EXPECT(f.takes_illegal_exception());
}

// --- Core ---------------------------------------------------------------------

void test_program_sequence() {
    Fixture f{
        0x2401,          // MOVE.L D1,D2
        0xD481,          // ADD.L D1,D2
        0x4EF9, 0x0000, 0x1000,  // JMP $1000
    };
    f.cpu.set_d(1, 21);
    f.cpu.step();
    f.cpu.step();
    EXPECT(f.cpu.d(2) == 42);
    f.cpu.step();
    EXPECT(f.cpu.pc() == kProgramBase);
}

// Fetching from an odd PC: address error, group 0 frame (14 bytes).
void test_odd_pc_fetch_is_address_error() {
    Fixture f{0x4ED0};  // JMP (A0) to an odd address
    f.bus->write32(3 * 4, 0x4800);  // address error vector
    f.cpu.set_a(0, 0x2001);
    f.cpu.step();
    EXPECT(f.cpu.step() == 50);
    EXPECT(f.cpu.pc() == 0x4800);
    EXPECT(f.cpu.a(7) == 0x8000 - 14);
    EXPECT(f.bus->read16(0x8000 - 14) == 0x16);      // read, instruction, supervisor program (FC 6)
    EXPECT(f.bus->read32(0x8000 - 12) == 0x2001);    // access address
    EXPECT(f.bus->read32(0x8000 - 4) == 0x2001);     // stacked PC
    EXPECT(f.cpu.exception_count(Cpu68000::kVectorAddressError) == 1);
}

// An address error while stacking an address error frame halts the 68000.
void test_double_bus_fault() {
    Fixture f{0x4ED0};
    f.cpu.set_a(0, 0x2001);
    f.cpu.step();
    f.cpu.set_a(7, 0x8001);  // odd supervisor stack: the frame can't be written
    try {
        f.cpu.step();
        EXPECT(false);
    } catch (const CpuError& e) {
        EXPECT(e.kind() == CpuError::Kind::DoubleBusFault);
    }
}

void test_illegal_instruction_frame() {
    Fixture f{0x2401, 0x4AFC};  // MOVE.L D1,D2 ; ILLEGAL
    f.cpu.step();
    f.cpu.set_sr(0x2000 | Cpu68000::kFlagC);
    EXPECT(f.cpu.step() == 34);
    EXPECT(f.cpu.pc() == 0x4000);
    EXPECT(f.cpu.a(7) == 0x8000 - 6);
    EXPECT(f.bus->read16(0x8000 - 6) == (0x2000 | Cpu68000::kFlagC));  // old SR
    EXPECT(f.bus->read32(0x8000 - 4) == kProgramBase + 2);             // the ILLEGAL itself
    EXPECT(f.cpu.exception_count(Cpu68000::kVectorIllegal) == 1);
}

void test_reset_reads_vectors_through_overlay() {
    auto bus = std::make_unique<MemoryBus>();
    std::vector<uint8_t> rom(MemoryBus::kRomSize);
    const uint8_t header[] = {0x11, 0x11, 0x4E, 0xF9, 0x00, 0xFC, 0x00, 0xD2};
    std::copy(std::begin(header), std::end(header), rom.begin());
    bus->load_kickstart(rom);

    Cpu68000 cpu{*bus};
    cpu.set_sr(0);  // start from user mode to check reset enters supervisor
    cpu.reset();
    EXPECT(cpu.sr() == 0x2700);
    EXPECT(cpu.supervisor());
    EXPECT(cpu.a(7) == 0x1111'4EF9);
    EXPECT(cpu.pc() == 0x00FC'00D2);
}

void test_sr_swaps_stack_pointers() {
    auto bus = std::make_unique<MemoryBus>();
    Cpu68000 cpu{*bus};  // starts in supervisor mode
    cpu.set_a(7, 0x0008'0000);  // SSP
    cpu.set_sr(0x0000);  // to user mode
    cpu.set_a(7, 0x0004'0000);  // USP
    EXPECT(cpu.ssp() == 0x0008'0000);
    EXPECT(cpu.usp() == 0x0004'0000);
    cpu.set_sr(0x2000);  // back to supervisor
    EXPECT(cpu.a(7) == 0x0008'0000);
    EXPECT(cpu.usp() == 0x0004'0000);

    cpu.set_sr(0xFFFF);  // unimplemented SR bits read as zero
    EXPECT(cpu.sr() == Cpu68000::kSrImplementedBits);
}

}  // namespace

void run_cpu68000_tests() {
    test_move_long_data_to_data();
    test_move_byte_preserves_upper_bits_and_sets_z();
    test_move_word_from_address_register();
    test_movea_word_sign_extends_without_flags();
    test_move_illegal_forms_trap();
    test_add_long_overflow();
    test_add_byte_carry_to_zero();
    test_add_word_negative_plus_negative();
    test_add_clears_stale_flags();
    test_adda_word_sign_extends_without_flags();
    test_addx_encoding_is_not_add();
    test_jmp_address_register_indirect();
    test_jmp_displacement();
    test_jmp_absolute();
    test_jmp_pc_relative();
    test_jmp_indexed();
    test_jmp_illegal_modes_trap();
    test_program_sequence();
    test_odd_pc_fetch_is_address_error();
    test_double_bus_fault();
    test_illegal_instruction_frame();
    test_reset_reads_vectors_through_overlay();
    test_sr_swaps_stack_pointers();
}
