# Amiga Emulator Project (C++ / CMake / SDL)

## Project Context & Architecture
Target: Minimal, clean, accurate **Amiga 500 (OCS)** hardware emulation.
Core stack: **C++20**, **CMake 3.22+**, **SDL3** (or SDL2).

### Component Architecture
1. **CPU**: Motorola 68000 (interpreter first, opcode decoding via lookup tables or bit-masking).
2. **Memory**: 512KB Chip RAM, 512KB Slow/Fast RAM, 256KB Kickstart ROM. Amiga Memory Map must be strictly respected.
3. **Custom Chips (Agnes/Denise/Paula)**: Shared registers interface, planar-to-chunky graphics conversion.
4. **Timing**: Cycle-accurate or instruction-aligned execution loops. Synchronized via Amiga's master clock frequencies (PAL: 7.09379 MHz).

## Build & Test Commands
- Build project: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
- Run emulator: `./build/amiga_emu`
- Run tests: `./build/tests_emu`

## Code Style & Rules
- **Modern C++**: Use strict type safety (`uint8_t`, `uint16_t`, `uint32_t`). Use `std::array` instead of raw arrays.
- **Performance**: Zero-allocation inside the main emulation loop (`step()` or `tick()`). Pass core structures by reference.
- **No Hallucinations**: Do not implement imaginary registers. Refer strictly to the official "Amiga Hardware Reference Manual".
- **Errors**: Panic and log when reading/writing to unmapped memory zones.
