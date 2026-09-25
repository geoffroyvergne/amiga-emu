#pragma once

#include <cstdint>

// Custom chip register offsets from $DFF000 (Amiga Hardware Reference Manual,
// Appendix A). Only registers that are actually emulated are listed.
namespace amiga::reg {

inline constexpr uint32_t kCustomBase = 0xDF'F000;

// Read-only.
inline constexpr uint16_t kDmaConR = 0x002;
inline constexpr uint16_t kVPosR = 0x004;   // LOF, Agnus ID, V8
inline constexpr uint16_t kVHPosR = 0x006;  // V7-V0, H8-H1
inline constexpr uint16_t kJoy0Dat = 0x00A;  // port 0 (mouse) counters: Y7-Y0 | X7-X0
inline constexpr uint16_t kJoy1Dat = 0x00C;  // port 1 counters
inline constexpr uint16_t kPotGoR = 0x016;   // POTINP: pot pin levels (DATRY/DATRX/DATLY/DATLX)
inline constexpr uint16_t kIntEnaR = 0x01C;
inline constexpr uint16_t kIntReqR = 0x01E;

// Copper (Agnus).
inline constexpr uint16_t kCopCon = 0x02E;
inline constexpr uint16_t kPotGo = 0x034;    // pot pin direction/data
inline constexpr uint16_t kJoyTest = 0x036;  // writes all mouse counters
inline constexpr uint16_t kCop1Lch = 0x080;
inline constexpr uint16_t kCop1Lcl = 0x082;
inline constexpr uint16_t kCop2Lch = 0x084;
inline constexpr uint16_t kCop2Lcl = 0x086;
inline constexpr uint16_t kCopJmp1 = 0x088;  // strobe
inline constexpr uint16_t kCopJmp2 = 0x08A;  // strobe

// DMA and interrupt control.
inline constexpr uint16_t kDmaCon = 0x096;
inline constexpr uint16_t kIntEna = 0x09A;
inline constexpr uint16_t kIntReq = 0x09C;

// Agnus: display window and data fetch.
inline constexpr uint16_t kDiwStrt = 0x08E;  // V7-V0 | H7-H0 (V8 = 0, H8 = 0)
inline constexpr uint16_t kDiwStop = 0x090;  // V7-V0 | H7-H0 (V8 = !V7, H8 = 1)
inline constexpr uint16_t kDdfStrt = 0x092;  // fetch start, color clocks
inline constexpr uint16_t kDdfStop = 0x094;  // fetch stop, color clocks

// Agnus: bitplane DMA pointers BPL1PT-BPL6PT ($0E0-$0F6, 4 bytes apart).
inline constexpr uint16_t kBpl1Pth = 0x0E0;  // bitplane 1 pointer, high bits (A18-A16)
inline constexpr uint16_t kBpl1Ptl = 0x0E2;  // bitplane 1 pointer, low bits (A15-A1)
inline constexpr uint16_t kBpl6Ptl = 0x0F6;
inline constexpr uint16_t kBpl1Mod = 0x108;  // odd bitplanes (1, 3, 5) modulo
inline constexpr uint16_t kBpl2Mod = 0x10A;  // even bitplanes (2, 4, 6) modulo

// Denise (BPLCON0 is also latched by Agnus): display control and palette.
inline constexpr uint16_t kBplCon0 = 0x100;
inline constexpr uint16_t kBplCon1 = 0x102;  // playfield scroll delays
inline constexpr uint16_t kBplCon2 = 0x104;  // playfield priorities
inline constexpr uint16_t kColor00 = 0x180;  // COLOR00-COLOR31: $180-$1BE
inline constexpr uint16_t kColor31 = 0x1BE;

// BPLCON0 bits.
inline constexpr uint16_t kBplCon0Hires = 1u << 15;
inline constexpr unsigned kBplCon0BpuShift = 12;  // BPU2-BPU0: number of bitplanes
inline constexpr uint16_t kBplCon0Homod = 1u << 11;  // hold-and-modify
inline constexpr uint16_t kBplCon0Dblpf = 1u << 10;  // dual playfield
inline constexpr uint16_t kBplCon0Color = 1u << 9;   // composite colour enable

// BPLCON2 bits.
inline constexpr uint16_t kBplCon2Pf2Pri = 1u << 6;  // playfield 2 in front of playfield 1

// SET/CLR bit shared by DMACON, INTENA, INTREQ (and ADKCON).
inline constexpr uint16_t kSetClr = 0x8000;

// DMACON bits.
inline constexpr uint16_t kDmaEn = 1u << 9;  // master enable
inline constexpr uint16_t kBplEn = 1u << 8;
inline constexpr uint16_t kCopEn = 1u << 7;
inline constexpr uint16_t kBltEn = 1u << 6;
inline constexpr uint16_t kDskEn = 1u << 4;

// INTENA / INTREQ bits, with their 68000 interrupt level.
inline constexpr uint16_t kIntEn = 1u << 14;   // INTENA master enable
inline constexpr uint16_t kIntExter = 1u << 13;  // L6: CIA-B
inline constexpr uint16_t kIntDskSyn = 1u << 12; // L5
inline constexpr uint16_t kIntRbf = 1u << 11;    // L5
inline constexpr uint16_t kIntAud3 = 1u << 10;   // L4
inline constexpr uint16_t kIntAud0 = 1u << 7;    // L4
inline constexpr uint16_t kIntBlit = 1u << 6;    // L3
inline constexpr uint16_t kIntVertb = 1u << 5;   // L3: start of vertical blank
inline constexpr uint16_t kIntCoper = 1u << 4;   // L3: set by the Copper (via INTREQ)
inline constexpr uint16_t kIntPorts = 1u << 3;   // L2: CIA-A
inline constexpr uint16_t kIntSoft = 1u << 2;    // L1
inline constexpr uint16_t kIntDskBlk = 1u << 1;  // L1
inline constexpr uint16_t kIntTbe = 1u << 0;     // L1

}  // namespace amiga::reg
