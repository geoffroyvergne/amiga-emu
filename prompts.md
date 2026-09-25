# Step 1: Project Skeleton (CMake + SDL + Main Loop)

Initialize the project structure according to our CLAUDE.md. Generate the CMakeLists.txt file featuring C++20 support and the SDL3 (or SDL2) dependency. Create a clean src/main.cpp that initializes an SDL window targeted at a 640x512 resolution. Implement a basic emulation execution loop tightly regulated to match PAL standards (50 FPS / 20ms frame time), including basic window close event handling.

# Step 2: The Memory Mapping (Bus & RAM/ROM)

Implement the MemoryBus class for our Amiga 500 emulation. It needs to manage a 512KB array for the Chip RAM (starting at address 0x000000) and a mechanism to load a 256KB binary Kickstart ROM file (mapped at 0xFC0000). Implement the core read8, read16, read32, write8, write16, and write32 methods. Ensure strict compliance with the Big-Endian byte ordering required by the Motorola 68000 CPU.

# Step 3: CPU Architecture & Core Decoding

Create the Cpu68000 class. Define the structural components of the 68000 processor: 8 Data registers (D0-D7), 8 Address registers (A0-A7), the Program Counter (PC), and the Status Register (SR). Implement a core step() function that fetches the opcode pointed to by the PC via the MemoryBus. Write the decoding and execution logic for basic opcodes: MOVE (register-to-register), ADD (register-to-register), and a basic jump JMP. Correctly update the Zero (Z) and Negative (N) flags in the Status Register.

# Step 4: Planar Graphics Conversion (Denise Chip)

We need to set up a rudimentary display pipeline based on the Denise custom chip. Write a method in a new Denise class that receives a pointer to the MemoryBus or Chip RAM. It must read the bitplane pointer registers (BPL1PTH/BPL1PTL), extract the raw data of 1 active bitplane, and convert this planar bitmap row into a standard 32-bit ARGB pixel array. This output array will be used to update our target SDL texture.

# Step 5: Validation, Debugging & Testing Loop

Implement a diagnostic logging mechanism or a unit test suite within the CPU module to print the state of all registers after every instruction execution cycle. Write a small helper function that injects a hardcoded array of raw m68k machine code instructions into our virtual RAM. This will allow us to safely verify that our CPU core executes arithmetic operations, loops, and condition flags accurately.

# Step 6: The interrupt engine and the Copper (the heart of the Amiga). 

The Amiga relies entirely on synchronization between the CPU and the Copper (a graphics coprocessor synchronized with the CRT beam). Now is the time to implement it.

Now that the basic CPU and memory loop work, let's implement the Copper (Coprocessor) and the Interrupt framework. Create a Copper class that reads its own instruction stream from Chip RAM via two registers: COP1LCH/COP1LCL. Implement the three basic Copper instructions: MOVE (write to a custom register), WAIT (wait for a specific beam position V, H), and SKIP. Integrate this into our main execution loop so that the Copper executes cycles in parallel with the CPU based on the beam position.

# Step 7: Large-scale CPU instruction decoding. 

To run actual Amiga code, your CPU must support virtually the entire Motorola 68000 instruction set, and especially the various addressing modes (indirect with displacement, pre-decrement, etc.).

We need to scale up our Cpu68000. Instead of writing opcodes manually, let's implement an optimized Opcode Dispatcher. Use a bitmask lookup table (or a large switch-case/function pointer array of 65536 entries) to decode instructions efficiently. Focus on implementing the missing core instructions required for booting: MOVE (all addressing modes), BRA, BSR, RTS, CMP, TST, and bit manipulation instructions like BTST.

# Step 8: The complete display pipeline (Amiga OCS Playfields). 

The Amiga does not simply display a single black-and-white bitplane; it handles up to 6 bitplanes, color palettes (colors 0 to 31 via registers COLOR00 to COLOR31), and hardware scrolling.

Let's upgrade our Denise and Agnus video generation. Implement Multi-planar rendering supporting up to 5 or 6 bitplanes (Dual Playfield / EHB modes). Create the color palette array using the COLOR00 to COLOR31 registers (12-bit RGB format, converted to 32-bit ARGB for SDL). Implement the horizontal and vertical blanking logic (DIWSTRT, DIWSTOP, DDFSTRT, DDFSTOP) to properly frame the display window.

# Step 9: Emulation of the CIA (Complex Interface Adapters) and Keyboard/Mouse. 

The Amiga uses two MOS 8520 chips (CIA-A and CIA-B) to manage timers, system clock events (50Hz/60Hz), the keyboard, the mouse, and joysticks. Without them, it is impossible to detect a key press or load a file.

Implement the CIA-A and CIA-B (MOS 8520) chip architectures. Map them to their respective memory zones (0xBFE000 and 0xBFD000). Implement the CIA timers (Timer A and Timer B) which decrement on every clock cycle and trigger CPU Interrupt Level 2 or Level 6 when they hit zero. Connect SDL keyboard and mouse events to the CIA registers so our emulated system can register user inputs.

# Duplicated ! Step 10: The first major boot (Kickstart and disk wait loop). 

The ultimate goal of this phase is to see the iconic Kickstart 1.3 "Insert Workbench Disk" screen appear.

Let's perform a milestone integration test. Put together the CPU, Memory, CIA Timers, and Video rendering. Boot the emulator using an authentic Kickstart 1.3 ROM image. Trace the instruction execution. If the CPU loops or crashes, output a detailed log showing the last 100 instructions and the state of custom registers. Our goal is to successfully reach the animated 'Insert Disk' prompt on screen.

# Duplicated ! Step 11: The Copper (Amiga's Graphics Coprocessor)

Create a Copper class to emulate the Amiga's graphics coprocessor. It must read its instruction stream from Chip RAM via COP1LCH/COP1LCL registers. Implement the three hardware instructions: MOVE (write to a custom register), WAIT (halt until the beam reaches a specific V/H position), and SKIP. Integrate this into the main execution loop so that the Copper executes cycles in sync with the video beam position.

# Step 12: Opcode Dispatcher & Core 68000 Instructions Expansion

We need to expand the Cpu68000 instruction set to handle real software. Implement an optimized Opcode Dispatcher using a 65536-entry lookup table (or an optimized bitmask switch). Implement the critical missing opcodes and their addressing modes required for system code: MOVE (all modes), BRA, BSR, RTS, CMP, TST, EXT, LINK, UNLK, and bit testing (BTST).

# Duplicated ! Step 13: OCS Multi-Bitplane Display & Color Palette

Upgrade the Denise video system. Implement Multi-planar rendering supporting up to 5 bitplanes. Read the color palette from custom registers COLOR00 through COLOR31 (convert the 12-bit Amiga RGB format to 32-bit ARGB for SDL). Implement the display window registers (DIWSTRT, DIWSTOP, DDFSTRT, DDFSTOP) to correctly clip and frame the 640x256/512 viewport.

# Step 14: CIA Timers (MOS 8520) & SDL Input Mapping

Implement the CIA-A (0xBFE000) and CIA-B (0xBFD000) chips. Focus on emulating Timer A and Timer B countdowns, which must trigger CPU Interrupt Level 2 or 6 upon reaching zero. Map SDL keyboard and mouse input events directly into the CIA registers and Joy0DAT/Joy1DAT addresses so the system can register user interactions.

# Step 15: Kickstart Execution & Debugging Diagnostics

Let's run a full boot test using a Kickstart 1.3 ROM image. Combine the CPU, Memory, CIA Timers, Copper, and Denise systems into the loop. Create a diagnostic tracer that logs a rolling buffer of the last 200 executed instructions and register states. If the CPU hits an illegal opcode or an unmapped memory access, dump this log to help us debug what hardware register or instruction is missing to reach the 'Insert Disk' screen.

# Step 16 : Check memory

Before we move to input handling, let's verify a critical Amiga hardware feature: the Kickstart Memory Overlay. Ensure that at reset, address 0x000000 maps to the Kickstart ROM (0xFC0000) so the CPU fetches the correct initial SP and PC. Then, ensure that writing to the CIA-A Port A register (0xBFE001, bit 0) toggles this overlay off, restoring the actual Chip RAM to 0x000000. If this isn't implemented, update the MemoryBus and CIA modules to support it.

# Step 17 : check CPU

Ensure our Cpu68000 core supports Autovectored Interrupts (Levels 1 to 6). Implement an assert_interrupt(int level) method in the CPU. When an interrupt is triggered (and if its level is higher than the current processor priority mask in the Status Register SR), the CPU must: push the current PC and SR onto the Supervisor Stack (A7), switch to Supervisor mode, and jump to the corresponding exception vector address (Level 1 = 0x64, Level 2 = 0x68, etc.).

# Step 18 : Keyboard / mouse interraction

Let's fully implement Keyboard and Mouse interaction. Map SDL input events to the Amiga architecture:

1- Keyboard: Amiga uses a serial shifting matrix. When an SDL key event occurs, convert the SDL scancode to the Amiga Raw Keycode. Stream this 7-bit code into the CIA-A Serial Data Register (SDR, 0xBFED00) and trigger a Level 2 Interrupt (PORTS) via the custom registers INTREQ/INTENA.

2- Mouse/Joystick: Map SDL mouse motion to the JOY0DAT (0xDFF00A) counter register, implementing the quadrature counters so the Kickstart can track the hardware mouse pointer.

# Step 19: Keyboard & Mouse Interaction (via the CIA-A chip) 

The Amiga handles the keyboard via a serial shift register on the CIA-A chip. For the system to respond, SDL keys must be converted into hardware Amiga scancodes and an interrupt triggered.

Let's connect SDL inputs to the Amiga hardware. Implement the keyboard and mouse logic:Keyboard: 

1- Create a mapping table from SDL Scancodes to Amiga Raw Keycodes. When a key is pressed or released, write the 7-bit Amiga code (with bit 7 set to 1 for key release) into the CIA-A Serial Data Register (SDR, 0xBFED00). Then, trigger a Level 2 CPU Interrupt (PORTS) by setting bit 3 in the custom registers INTREQ and INTENA.

2- Mouse: Map SDL mouse movement (SDL_EVENT_MOUSE_MOTION) to the JOY0DAT (0xDFF00A) register using quadrature encoding (X and Y counters) so the Kickstart hardware can move the operating system pointer.

# Step 20: Floppy Disk Drive Emulation (Trackdisk & ADF Files) 

To play games or run demos, you need the floppy disk drive (DF0:). The Amiga reads disks via the Agnus chip's DMA controller and the CIA-B chip's select signals (drive selection, motor direction, track 0 detection). The standard format is .adf (Amiga Disk File, 901,120 bytes).

We need to emulate the floppy disk drive (DF0:) to load Amiga Disk Files (.adf).

1- Implement a FloppyDrive component. Map its control lines (Drive Select, Motor On, Step, Side Select) to CIA-A Port B (0xBFE101) and CIA-B Port A (0xBFD000) registers.

2- Implement the read pipeline. When the Amiga triggers a Floppy DMA (DSKDAT / DSKPTH / DSKPTL registers), stream the raw MFM encoded data from the loaded .adf file into the Chip RAM, and trigger a Level 5 CPU Interrupt (DISK) when the transfer completes.

3- Add a basic command-line parameter or SDL drag-and-drop mechanism to load an .adf file at startup.