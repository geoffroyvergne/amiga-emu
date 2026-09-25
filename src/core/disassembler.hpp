#pragma once

#include <cstdint>
#include <string>

#include "core/memory_bus.hpp"

namespace amiga {

struct Disassembly {
    std::string text;     // e.g. "MOVE.L  #$00020000,D0"
    uint32_t length = 2;  // bytes, including extension words
};

// 68000 disassembler for debugging (Motorola syntax). Reads through
// MemoryBus::peek16, so it never touches I/O registers. Allocates: not for
// use inside the emulation loop.
[[nodiscard]] Disassembly disassemble(const MemoryBus& bus, uint32_t pc);

}  // namespace amiga
