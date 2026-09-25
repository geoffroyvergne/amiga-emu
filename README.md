# Amiga 500 Emulator

A minimal, clean Amiga 500 (OCS, PAL) emulator in C++20, built with CMake and SDL3.
It boots Kickstart 1.3 and Workbench 1.3 and runs games from ADF disk images.
Tested so far with Shadow of the Beast, Saint Dragon, Shufflepuck Café, Prince of Persia, Flashback and R-Type II.

- [Building](#building)
- [Running](#running)
- [Controls](#controls)
- [Strategy](#strategy)
- [Architecture](#architecture)
- [Development steps](#development-steps)
- [Diagnostics](#diagnostics)
- [Limitations](#limitations)

## Building

Requirements:

- a C++20 compiler (Clang, GCC or MSVC);
- CMake 3.22 or later;
- SDL3 (on macOS: `brew install sdl3`).

```sh
# Debug build (the default)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Release build: use it to play, the Debug build is much slower
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

This produces three targets:

| Target       | What it is                                                             |
|--------------|------------------------------------------------------------------------|
| `amiga_core` | Static library with the whole machine. It has no SDL dependency, so it can be tested on its own. |
| `amiga_emu`  | The SDL3 front end: window, audio, keyboard, mouse and gamepad.       |
| `tests_emu`  | The unit test suite.                                                   |

### Tests

```sh
./build/tests_emu
```

The Kickstart boot test is skipped unless you point it at a Kickstart 1.3 ROM:

```sh
AMIGA_KICKSTART_13=/path/to/kick34005.rom ./build/tests_emu
```

## Running

```sh
./build-release/amiga_emu [options] [kick.rom]
```

You must supply a 256 KB Kickstart ROM image yourself; Kickstart 1.3 (34.5) is the one tested. Without a ROM, the emulator shows a test pattern.

Examples:

```sh
# Boot to the "insert disk" screen
./build-release/amiga_emu ~/roms/kick13.rom

# Boot a game
./build-release/amiga_emu --df0 "R-type 2.adf" ~/roms/kick13.rom

# Joystick on the keyboard from the start
./build-release/amiga_emu --joy-keys --df0 game.adf ~/roms/kick13.rom

# Run 3000 frames without a window and save the last one
./build-release/amiga_emu --headless --frames 3000 --screenshot out.bmp --df0 game.adf ~/roms/kick13.rom
```

### Options

| Option              | Effect |
|---------------------|--------|
| `--df0 FILE.adf`    | Insert a disk in DF0 at start-up. |
| `--turbo-floppy`    | Make disk DMA about 113× faster. By default the disk is read at the real speed, and the emulation runs unthrottled (and silent) while a disk is being read, so loading is still quick. Turbo loads faster, but some loaders fail with it (Oh No! More Lemmings). |
| `--joy-keys`        | Start with the keyboard joystick on (toggle it with F12+J). |
| `--no-slow-ram`     | Emulate a stock 512 KB A500. By default the machine also has 512 KB of trapdoor slow RAM at `$C00000`. |
| `--headless`        | Run without a window, as fast as possible. At the end it prints a state dump. |
| `--frames N`        | Stop after N frames. The headless default is 500. |
| `--screenshot FILE` | Save the last frame as a 640×512 BMP on exit. |
| `--no-trace`        | Don't record the last instructions. Recording is on by default and the trace is dumped on a crash, a hang or headless exit. |
| `--log-input`       | Log keyboard, mouse, gamepad, window-focus and mouse-capture events, to troubleshoot input problems. |

The headless exit code is 0 when the run is normal, 2 when the CPU halted, and 1 on an error.

### Disks

There are three ways to insert a disk:

- pass `--df0 FILE.adf` on the command line;
- drag and drop an `.adf` file onto the window;
- hold **F12** and press **F1**, **F2** or **F3** to load `disk1.adf`, `disk2.adf` or `disk3.adf` from the emulator's directory.

A disk swap leaves the drive empty for about 3 seconds before the new disk goes in, like a person swapping disks by hand. Some games only notice a new disk after seeing the drive empty.

Disks are read-only: the drive reports them as write-protected, and data written to them is discarded.

## Controls

The machine is wired like a real A500:

- **port 1** holds the mouse;
- **port 2** holds the joystick.

### Keyboard

| Host key                                   | Amiga                         |
|--------------------------------------------|-------------------------------|
| Letters, digits, punctuation, keypad, arrows, F1–F10 | The same keys (by physical position, US layout) |
| Esc, Tab, Return, Backspace, Delete        | The same keys                 |
| Left / Right Shift, Caps Lock              | Shift, Caps Lock              |
| Left or Right Ctrl                         | Ctrl (the A500 has only one)  |
| Left / Right Alt                           | Left / Right Alt              |
| Left / Right Cmd (Windows key)             | Left / Right Amiga            |
| F11                                        | Help                          |

Emulator keys:

| Keys           | Action |
|----------------|--------|
| F12            | Release the mouse. F12 itself is never sent to the Amiga. |
| F12 + J        | Turn the keyboard joystick on or off. |
| F12 + F1/F2/F3 | Load `disk1.adf`, `disk2.adf` or `disk3.adf`. F1–F3 pressed alone go to the Amiga. |
| Close window   | Quit. Esc is an Amiga key, so it doesn't quit. |

The Amiga does its own key repeat, so the host's key repeat is ignored.

### Keyboard joystick (port 2)

The keyboard joystick is off by default. Turn it on with **F12+J** or `--joy-keys`. The window title shows when it is on.

| Keys                          | Joystick  |
|-------------------------------|-----------|
| Arrow keys or W/A/S/D         | Directions |
| Left Ctrl, Space or Left Alt  | Fire       |

While the keyboard joystick is on, these keys control the joystick only and no longer reach the Amiga keyboard. Pressing two opposite directions at once cancels them out, as on a real joystick.

### Mouse (port 1)

- **Click in the window** to capture the mouse. That first click is not passed to the Amiga.
- **Press F12** to release it.
- Losing the window's focus also releases the mouse and every held key and button.

Left, right and middle buttons map to the Amiga mouse buttons. One Amiga pixel of motion equals two host pixels.

### Gamepad

The first gamepad connected becomes the port 2 joystick. You can plug it in while the emulator is running.

| Gamepad button                       | Amiga |
|--------------------------------------|-------|
| D-pad or left stick                  | Joystick directions. The stick counts once pushed past half its travel. |
| South (A on Xbox, Cross on PlayStation, **B** on Nintendo) | Fire |
| East (B / Circle / A)                | Space |
| West (X / Square / Y)                | Y key (for "yes" prompts) |
| Start / +                            | Return + Space (skips most intros) |
| Back / Select / −                    | Esc |
| Left shoulder                        | Left mouse button |
| Right shoulder                       | Right mouse button |

SDL names buttons by their position, not their label. On a Nintendo controller, fire is therefore the bottom button, which is labelled **B**.

Gamepad and keyboard inputs are merged, so a key held from both is released only when both let go.

## Strategy

The project follows these rules (see [CLAUDE.md](CLAUDE.md)):

- **Accuracy from the documentation.** Every register and behaviour follows the *Amiga Hardware Reference Manual*, the 8520 CIA datasheet and the 68000 user's manual. No invented registers. When something isn't emulated, the code says so, and access to unemulated registers is logged.
- **Fail loudly.** Access to unmapped memory panics and logs with the address. The CPU takes a proper bus or address error exception.
- **An SDL-free core.** All of the machine lives in `amiga_core`, and the front end only turns SDL events into `Machine` calls. As a result, the whole machine is unit-testable and can run headless.
- **Instruction-aligned timing.** The CPU runs one instruction, then the chipset catches up colour clock by colour clock. This makes CPU and chipset timing correct to within one instruction, which is enough for the Copper, raster effects and loaders, at a fraction of the cost of cycle-exact emulation.
- **Zero allocation in the loop.** Memory, frame buffer and audio buffers are allocated once. `run_frame()` never allocates.
- **Strict types.** The code uses `uint8_t`/`uint16_t`/`uint32_t`, `std::array` and `std::span` throughout.
- **Test first, then real software.** Each component has unit tests. The code is then checked against Kickstart, Workbench and real games. Each game bug is traced with the diagnostics tools down to the hardware detail being missed (examples: unaligned disk sync words, interrupt latency, CIA serial output), fixed by following the documentation, and covered by a regression test.

## Architecture

```
src/
  main.cpp                 SDL3 front end: window, pacing, audio, input, headless runner
  frontend/
    sdl_keymap.hpp         SDL scancode to Amiga raw keycode
    keyboard_joystick.hpp  keys acting as a digital joystick
    gamepad_mapping.hpp    gamepad buttons to Amiga keys and mouse buttons
  core/
    machine.*              the whole A500: owns everything and runs frames
    timing.hpp             PAL clocks, frame geometry, 50 Hz frame pacer
    memory_bus.*           24-bit address map, overlay, bank table
    cpu68000.*             Motorola 68000 interpreter
    disassembler.*         68000 disassembler (diagnostics)
    debug_dump.*           state dumps, deadlock reports
    chipset.*              custom-chip register file and per-colour-clock scheduling
    agnus.hpp              beam counter, DMACON, bitplane DMA
    denise.*, sprites.hpp  playfields, sprites, collisions, mouse and joystick counters
    copper.*               Copper coprocessor
    blitter.*              Blitter: area, fill and line modes
    paula.hpp              interrupts, pots and buttons
    paula_audio.hpp        four audio channels
    disk_controller.hpp    disk DMA, sync-word detection
    cia.*                  two 8520 CIAs as the A500 wires them
    keyboard.hpp           keyboard microcontroller protocol
    floppy.hpp, adf.*      DF0 drive mechanics, ADF to MFM track encoding
    custom_registers.hpp   register offsets and bit names
tests/                     unit tests, one file per component
```

### Timing

| Quantity            | PAL value |
|---------------------|-----------|
| Master clock        | 28.37516 MHz |
| CPU clock           | 7.09379 MHz |
| Colour clock (CCK)  | 3.546895 MHz, 1 CCK = 2 CPU cycles |
| Line                | 227 CCK |
| Frame               | 313 lines = 142,102 CPU cycles, 50 Hz |
| E clock (CIAs)      | CPU clock / 10, i.e. 1 tick every 5 CCK |

`Machine::run_frame()` runs one frame by alternating two steps:

1. the CPU executes one instruction and reports how many cycles it took;
2. the chipset advances the same number of cycles:
   - each colour clock ticks the beam, bitplane and sprite DMA, the Copper, the Blitter, audio and the floppy;
   - every 5 colour clocks, the CIAs and keyboard tick.

The CPU sees the interrupt level with one instruction of delay, as on hardware.

The front end paces frames at 20 ms and feeds about 960 stereo samples per frame to SDL at 48,007 Hz.

### Memory map

| Range               | Contents |
|---------------------|----------|
| `$000000–$1FFFFF`   | Chip RAM, 512 KB, repeated 4 times. At reset the ROM overlays this area, controlled by CIA-A PA0 (OVL). |
| `$BF0000–$BFFFFF`   | CIA-A (odd addresses, `$BFE001`) and CIA-B (even addresses, `$BFD000`) |
| `$C00000–$C7FFFF`   | Slow RAM, 512 KB (disable it with `--no-slow-ram`) |
| `$C80000–$DFFFFF`   | Custom chip registers, repeated. The canonical copy is at `$DFF000`. |
| `$E80000–$EFFFFF`   | Autoconfig space, empty: open bus |
| `$F00000–$F7FFFF`   | Empty: open bus |
| `$F80000–$FFFFFF`   | Kickstart ROM, 256 KB, repeated |
| Anything else       | Unmapped: triggers a bus error and a log line |

The bus decodes addresses through a 256-entry bank table (64 KB per bank) and uses big-endian byte order.

### Components

**CPU (68000)**
- A 65,536-entry handler table, built at start-up, decodes every opcode.
- One generic engine computes the effective address for all 12 addressing modes.
- It implements the full 68000 instruction set, including BCD, MOVEP, MOVEM, the multiply and divide instructions, and the exceptions: bus and address error (group 0, 14-byte stack frame), illegal instruction, Line A/F, privilege violation, TRAP, TRAPV and CHK.
- Interrupts are autovectored. A double bus fault halts the CPU.
- Instructions report approximate 68000 cycle counts.

**Agnus**
- Beam counter (VPOSR/VHPOSR).
- DMA control (DMACON).
- Bitplane DMA driven by DDFSTRT and DDFSTOP, fetching in 8-CCK units, both lowres and hires.
- Sprite DMA.

**Denise**
- Planar-to-chunky conversion of 1 to 6 bitplanes.
- 32-colour palette with 12-bit to ARGB conversion.
- Display window (DIWSTRT and DIWSTOP).
- Horizontal scrolling (BPLCON1) and modulos.
- Display modes: Extra Half-Brite, dual playfield and HAM.
- Eight sprites, including attached sprites, with playfield priorities.
- Sprite changes in the middle of a line are replayed at the right beam position.
- Collision detection (CLXCON and CLXDAT).
- Mouse and joystick counters (JOY0DAT and JOY1DAT).

**Copper**
- MOVE, WAIT and SKIP, using the beam comparator with enable masks.
- COP1LC and COP2LC, with jumps through COPJMP1 and COPJMP2.
- COPCON danger bit.

**Blitter**
- Area mode with the A, B and C channels, minterms, shifts, masks and modulos.
- Descending mode.
- Inclusive and exclusive fill.
- Line mode (octants, SING).
- The BLIT interrupt.

**Paula**
- INTENA and INTREQ, mapped to the 68000 interrupt levels.
- POTGO and POTGOR for the right and middle mouse buttons.
- Four DMA audio channels, plus manual mode, mixed as Amiga stereo (channels 0 and 3 on the left, 1 and 2 on the right).
- Disk controller:
  - DSKLEN double-write start;
  - bit-level sync-word detection with WORDSYNC realignment;
  - DSKBYTR;
  - DSKSYNC and DSKBLK interrupts.

**CIAs (8520)**
- Ports and data-direction registers.
- Timers A and B: continuous and one-shot modes, and timer B counting timer A underflows.
- Time-of-day counter with alarm.
- Interrupt control register (ICR).
- Serial port: input from the keyboard, and output shifting clocked by timer A, which some games use for the keyboard handshake.
- The A500 wiring:
  - CIA-A drives the overlay, the fire buttons, the floppy status lines and the keyboard, and raises interrupt level 2;
  - CIA-B drives the floppy control lines and receives the disk index pulse on FLAG, and raises interrupt level 6.

**Keyboard**
- The keyboard microcontroller protocol:
  - key codes sent as `~((code << 1) | up)`;
  - the handshake pulse;
  - the power-up codes.

**Floppy (DF0)**
- A double-density 3.5" drive:
  - motor, direction and step lines;
  - track 0, disk change and ready signals;
  - an index pulse on every rotation at 300 RPM.
- ADF images are encoded as standard AmigaDOS MFM tracks, the format the trackdisk loader expects.

### Front end

- **Window:** 640×512 and resizable. It shows 640×256 lowres pixels, each doubled vertically, with nearest-neighbour scaling and letterboxing.
- **Pacing:** 50 Hz from the emulator's own frame pacer, with vsync off.
- **Warp:** while the disk is being read, and for 3 seconds after, frames run unthrottled (and silent) and the screen refreshes at most 50 times a second. The disk itself turns at the real speed, so loaders see real timing, but a load that takes 20 s on an A500 finishes in about 2 s.
- **Hang detection:** a CPU halt, or a CPU stuck in a tight loop for 5 seconds, prints a full state dump once.

## Development steps

The emulator was built in these stages:

1. **Skeleton.** CMake with C++20 and SDL3, a 640×512 window and a 50 Hz PAL frame loop.
2. **Memory bus.** Chip RAM, the Kickstart ROM at `$FC0000`, big-endian 8/16/32-bit access, and the reset overlay.
3. **CPU core.** Registers, SR, fetch and decode, and the first instructions (MOVE, ADD, JMP) with condition codes.
4. **Denise, first version.** One bitplane converted to ARGB and shown in the SDL texture.
5. **Test harness.** Unit tests that load raw 68000 machine code into RAM and check registers and flags.
6. **Copper and interrupts.** Copper MOVE, WAIT and SKIP synchronised with the beam, plus INTENA and INTREQ and the interrupt levels.
7. **Opcode dispatcher.** The 65,536-entry table, every addressing mode, and branches, BSR/RTS, CMP, TST and bit instructions.
8. **Full playfields.** Up to 6 bitplanes, the 32-colour palette, EHB and dual playfield, and DIW/DDF windowing.
9. **CIAs and input.** Two 8520s with timers, TOD and ICR, the keyboard protocol, and the mouse and joystick counters.
10. **Complete 68000 instruction set** and exception processing.
11. **Kickstart 1.3 boot.** Diagnostics (trace, state dump, disassembler) used to reach the "insert disk" screen.
12. **Memory map fixes.** Chip RAM mirrors, custom-chip mirrors, open-bus areas and slow RAM, which all let Kickstart size memory correctly.
13. **Blitter.** Area, fill and line modes (Kickstart draws the insert-disk animation with it).
14. **Floppy.** ADF loading, MFM encoding, the drive mechanics and disk DMA. Workbench 1.3 boots.
15. **Disk hotkeys and drag and drop**, with realistic disk swaps.
16. **Audio.** Paula's four channels, sent to an SDL audio stream.
17. **Turbo floppy and warp.** Fast loading. Turbo DMA later became opt-in: warp alone keeps loads quick with real disk timing.
18. **Sprites.** The Workbench pointer, attached sprites, priorities, mid-line changes and collision detection.
19. **Host input.** Keyboard joystick, native gamepad support and gamepad button mapping.
20. **Diagnostics engine.** Instruction trace ring, deadlock detector, hints in state dumps, and the slow RAM switch.
21. **Game compatibility fixes.** Each fix found through a real game:
    - **Prince of Persia:** bit-level disk sync, and DSKBLK timing in turbo mode.
    - **Flashback:** interrupt latency, live CIA-A floppy status, and the empty drive during a disk swap.
    - **R-Type II:** warp tied to actual disk reads, and CIA serial output for the keyboard handshake.
    - **Shadow of the Beast:** COPJMP strobes on read, and a clearer message for its deliberate address errors (protection code).
    - **Barbarian:** write-only registers read as `$FFFF`, as on OCS. The game's `ORI.W #$8020,$DFF096` depends on it to switch DMA on.
    - **Oh No! More Lemmings:** real-speed disk timing by default. With turbo, its track loader sometimes started between a sector's two sync words, lost a sector, and crashed.

## Diagnostics

- **Trace ring.** The last instructions executed are recorded and included in every state dump. Turn it off with `--no-trace`.
- **State dump.** It shows:
  - the CPU registers and exception counters;
  - every custom register written;
  - the Copper state;
  - both CIAs and the overlay.
- **Deadlock detector.** It reports a loop of one or two instructions that keeps running with the registers unchanged, and suggests what the code is probably waiting for. Examples: a disk interrupt that never comes, a CIA bit, the Blitter.
- **Headless mode.** `--headless --frames N --screenshot out.bmp` reproduces a run without a window, which is useful for scripted testing.
- **`--log-input`** shows exactly which host events reach the emulator.

## Limitations

- Only PAL OCS A500 hardware: no ECS or AGA chips, no NTSC, one floppy drive (DF0).
- Timing is instruction-aligned, not cycle-exact:
  - DMA doesn't steal cycles from the CPU;
  - CIA accesses have no E-clock wait states;
  - the Blitter finishes each blit at once (BBUSY is never seen set).
- Disk writes are not emulated.
- Not emulated either:
  - interlace;
  - the audio low-pass filter;
  - attached audio channels;
  - the pot counters (analogue joysticks);
  - CIA CNT counting.
- Only ADF disk images are supported: no IPF or copy-protected formats.
