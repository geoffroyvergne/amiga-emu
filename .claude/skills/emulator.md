---
name: AmigaHardwareExpert
description: Generates ultra-precise Amiga 500 hardware structures, 68000 opcode handling, and bitwise custom chip registers manipulation.
when_to_use: When creating, optimizing, or debugging CPU opcode execution, memory mappings, bitplanes decoding, or Paula audio registers.
---

## Core Guidelines for Hardware Implementation

### 1. M68000 CPU Execution
- Implement opcodes with explicit handling of Status Register (SR) flags: Carry (C), Overflow (V), Zero (Z), Negative (N), Extend (X).
- Group operations logically (e.g., Data Movement, Integer Arithmetic, Bit Manipulation).

### 2. Amiga Planar Graphics (Denise / Agnus)
- Amiga uses planar bitmap memory (up to 6 bitplanes). 
- Always convert planar bytes to a standard ARGB8888 32-bit pixel buffer for SDL rendering.
- Example pattern for planar decoding:
  ```cpp
  // 1 pixel from 4 bitplanes (planar to chunky)
  uint8_t pixel_color_index = 0;
  for (int bp = 0; bp < num_bitplanes; ++bp) {
      pixel_color_index |= ((bitplane_data[bp] >> (7 - bit_idx)) & 1) << bp;
  }
  ```

### 3. SDL Integration
- Keep the rendering context separate from hardware logic. 
- Use `SDL_UpdateTexture` and `SDL_RenderCopy` outside the execution cycles to maintain 50Hz (PAL) / 60Hz (NTSC) limits.
