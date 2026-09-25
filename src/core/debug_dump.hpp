#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "core/machine.hpp"

namespace amiga {

// Name of a custom chip register ($DFF000 + offset), e.g. "DMACON", "COLOR05".
[[nodiscard]] std::string custom_register_name(uint16_t offset);

// Writes a post-mortem report: the last `instructions` traced instructions
// (disassembled, with the registers each one changed), CPU registers,
// exception counts, every custom register written so far, Copper and beam
// state, and both CIAs. Debug only: allocates.
void dump_state(Machine& machine, std::FILE* out, size_t instructions = 100);

}  // namespace amiga
