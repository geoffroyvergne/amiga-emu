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

# Step 13: Basic Audio Emulation (Paula Chip) 

The Amiga has 4 hardware audio channels (2 left, 2 right) managed by the Paula chip. Each channel plays samples (8-bit signed) directly from Chip RAM via DMA.

Let's implement rudimentary audio using the Paula custom chip and SDL Audio. Create 4 audio channels. Each channel has registers for sample pointers (AUDxLCH/AUDxLCL), length (AUDxLEN), period/pitch (AUDxPER), and volume (AUDxVOL). Set up an SDL audio callback queue. Inside our main emulation loop, when Audio DMA is active, fetch 8-bit signed samples from Chip RAM according to the channel's period, mix the 4 channels, and feed the buffer to SDL Audio.

# Step 14: drag and drop disk file change

Let's implement Keyboard/Mouse inputs and the Floppy Disk Drive (DF0:) architecture, including an easy way to swap disk files (.adf).

1- Disk Swapping Mechanism:

1.1- In the SDL event loop, catch SDL_EVENT_DROP_FILE. When a file is dropped onto the window, free the current disk image and load the new .adf file path into our Floppy Drive component.

1.2- Alternatively, map the F1, F2, and F3 keys to instantly cycle or load predefined disk files (e.g., disk1.adf, disk2.adf) from the execution directory for rapid testing.

2- Floppy Drive (DF0:) Core:

2.1- Create a basic FloppyDrive component holding the 901,120 bytes of an ADF file (80 tracks * 2 sides * 11 sectors * 512 bytes).

2.2- Connect drive control signals (Motor On, Step, Side, Track 0 detection) to the CIA-A Port B (0xBFE101) and CIA-B Port A (0xBFD000) registers.

2.3- When Floppy DMA is triggered via custom registers DSKPTH/DSKPTL and DSKLEN, stream the requested track data into Chip RAM and trigger a Level 5 CPU Interrupt (DISK) upon completion.

3- Keyboard & Mouse Inputs:

3.1- Map SDL keyboard events to Amiga Raw Keycodes. Write the code into the CIA-A Serial Data Register (SDR, 0xBFED00) and trigger a Level 2 CPU Interrupt (PORTS).

3.2- Map SDL mouse motion to the JOY0DAT (0xDFF00A) quadrature counter register so the Workbench/Kickstart mouse pointer can move.

# Fix workbench load : 

I successfully implemented the Drag & Drop mechanism and inserted a Workbench 1.3 ADF disk. However, the emulator froze, and the display output became heavily corrupted/degraded. This indicates that the CPU is executing garbage data or the graphics subsystem is misconfigured during disk reading.

Let's fix this by addressing the most common root causes:

1- Amiga MFM Floppy Encoding Check: An Amiga hardware disk controller expects track data to be MFM encoded, not raw sector data. If our FloppyDrive component dumps raw ADF sectors straight into RAM, the Amiga OS reads corrupted instructions. If we haven't implemented an MFM encoder/decoder, update the floppy DMA simulation to either map raw sectors to simulated MFM sync marks (0xAAAA / 0x4489) or implement a high-level bypass so the OS receives valid executable bytes in Chip RAM.

2- Blitter Check: Workbench heavily relies on the Blitter custom chip (BLTCON0, BLTSIZE, etc.) to draw icons and windows. If the Blitter is missing or blocking execution, the screen freezes. Add a basic stub or synchronized execution for Blitter registers if they are being written to.

3- CPU Post-Mortem Logging: When the freeze occurs, print a precise debug dump in the terminal showing:

3.1- The last 50 executed instructions (PC and opcode name).

3.2- The current state of INTREQ (Interrupt Request) and DMACON (DMA Control) registers.

3.3- Whether the CPU encountered an Illegal Opcode, an Address Error (unaligned 16/32-bit access), or a Double Bus Fault.

Let's analyze the logs or fix the Floppy DMA loop to ensure the code executed from the disk is accurate.

# Boost CPU and add sound

The Workbench 1.3 desktop now successfully boots and renders on screen! However, the emulation speed is quite slow during disk loading and system execution. Let's implement two final upgrades to make the emulator fully usable:

1- Performance & Speed Optimization:

1.1- Floppy Fast Forward: Implement a 'Turbo' flag for the disk drive. When Floppy DMA is active (DSKLEN is processing data), temporarily bypass the realistic sector delays to transfer the data instantly into Chip RAM. This will make Workbench boot in a few seconds.

1.2- Compiler Optimization Checks: Ensure that inline functions used for memory operations (read16, write16) and flag updates in the Cpu68000 class are correctly marked as inline or constexpr, allowing the compiler to optimize the hot paths.

2- Audio Implementation (Paula Custom Chip):

2.1- Let's add sound using SDL Audio (SDL_AudioStream or Callback).

2.2- Implement the hardware registers for the 4 audio channels: AUDxLCH/LCL (Sample Pointer), AUDxLEN (Length), AUDxPER (Period/Pitch), and AUDxVOL (Volume, 0-64).

2.3- In our main execution loop, when Audio DMA is enabled in DMACON, stream 8-bit signed samples from Chip RAM, modulate them according to their channel period, and mix them together into a standard stereo floating-point or 16-bit buffer for SDL Audio.

Let's implement these two features to complete our core operational Amiga 500 emulator.

# Keyboard and mouse

The fast boot is working perfectly! Now, let's implement the complete Keyboard and Mouse user interactions so we can fully navigate the Workbench desktop.

1- Hardware Mouse Emulation (Agnus Quadrature):

1.1- Catch SDL_EVENT_MOUSE_MOTION in the main SDL event loop.

1.2- Map relative mouse movements (event.motion.xrel and yrel) to the JOY0DAT (0xDFF00A) register. Amiga tracks the mouse using an 8-bit horizontal counter (low byte) and an 8-bit vertical counter (high byte) that increment/decrement using quadrature encoding.

1.3- Map Left Mouse Button clicks to CIA-A Port A, bit 6 (0xBFE001). A value of 0 means pressed, 1 means released.

1.4- Map Right Mouse Button clicks to the custom register POTGO (0xDFF016, bit 10 or 14) as expected by the OCS hardware.

2- Hardware Keyboard Emulation (CIA-A Serial Transfer):

2.1- Create a mapping table from SDL Scancodes to Amiga Raw Keycodes (e.g., Space = 0x40, Return = 0x44, Escape = 0x45).

2.2- When an SDL key event occurs (SDL_EVENT_KEY_DOWN or SDL_EVENT_KEY_UP):

2.2.1- Format the 7-bit Amiga keycode. For key release (KEY_UP), set bit 7 to 1 (e.g., keycode | 0x80).

2.2.2- Load this byte into the CIA-A Serial Data Register (SDR, 0xBFED00).

2.2.3- Set bit 3 (PORTS) in the custom interrupt registers INTREQ (0xDFF09C) to trigger a Level 2 CPU Interrupt, signaling the Amiga OS that a key matrix change occurred.

Let's integrate this input mapping layer into our main execution loop so we can move the cursor and click on icons inside Workbench.

# keyboard in games

The keyboard works perfectly on the Workbench, and the mouse works in games, but I cannot play games because they require an Amiga Joystick plugged into Port 2.

Let's implement Joystick Port 2 Emulation mapped to the host keyboard arrow keys (or WASD):

1- Joystick Registers Configuration:

1.1- Amiga tracks Joystick 2 via the JOY1DAT (0xDFF00C) register. Like the mouse, it uses quadrature counters where the bits represent directions.

1.2- Specifically, bit 1 is Right, bit 0 is (Left XOR Right), bit 9 is Down, and bit 8 is (Up XOR Down).

1.3- The Joystick 2 Fire Button is read from CIA-A Port A, bit 7 (0xBFE001). A value of 0 means pressed, 1 means released.

2- SDL Input Mapping:

2.2- Inside our SDL event loop, intercept when the user presses Arrow Keys (Up, Down, Left, Right) or WASD. Modify the bits of JOY1DAT accordingly to simulate physical joystick directional movements.

2.3- Map the Left Ctrl key, Spacebar, or Left Alt key to act as the Joystick Fire Button, changing bit 7 of 0xBFE001.

Let's inject this Joystick emulation layer into our project so that commercial games can register movement and fire inputs.

# complete input mapping

Let's complete our input mapping system by implementing a comprehensive fallback bridge between the SDL Gamepad buttons and the Amiga Keyboard / Mouse subsystem. Many Amiga games require specific keyboard presses or mouse clicks to bypass intros, select options, or access submenus.

Map the remaining physical Gamepad buttons to the following emulated Amiga hardware states:

1- Menu / Intro Bypass (Start & Select Buttons):

1.1- Map the Gamepad START button (SDL_GAMEPAD_BUTTON_START) to trigger the Amiga Return / Enter Key (0x44 on the keyboard matrix) and Spacebar (0x40) simultaneously or conditionally, as these are universally used to skip intros.

1.2- Map the Gamepad BACK / SELECT button (SDL_GAMEPAD_BUTTON_BACK) to trigger the Amiga Escape Key (0x45), which is often used to abort cinematic sequences.

2- Secondary Controls (Face & Shoulder Buttons):

2.1- Map the Gamepad EAST button (B on Xbox / Circle on PS) to trigger Spacebar (0x40) or act as a secondary action button.

2.2- Map the Gamepad WEST button (X on Xbox / Square on PS) to trigger the Amiga 'Y' or 'N' keys (useful for instant confirmation prompts in games).

3- Mouse Click Emulation via Shoulder Buttons (For Trainer/Cracktro menus):

3.1- Map the Gamepad Left Shoulder (LB/L1) to act as an Amiga Left Mouse Click (setting bit 6 of CIAAPRA to 0).

3.2- Map the Gamepad Right Shoulder (RB/R1) to act as an Amiga Right Mouse Click (modifying the POTGO register).

3.3- This allows navigating text-based cracktros or game configuration launchers using only the controller.

Please implement this expanded mapping layout cleanly within our existing SDL input polling thread.

# Joypad in game

Let's add native support for physical USB / Bluetooth Gamepads using the SDL Gamepad API, mapping it directly to the Amiga's Joystick Port 2.

1- Initialization:

1.1- In our SDL initialization code, call SDL_Init(SDL_INIT_GAMEPAD) (or SDL_INIT_GAMECONTROLLER if using SDL2).

1.2- Handle gamepad connection and disconnection events (SDL_EVENT_GAMEPAD_ADDED and SDL_EVENT_GAMEPAD_REMOVED) to automatically open the first available physical gamepad using SDL_OpenGamepad.

2- Input Mapping to Amiga Registers:

2.1- D-Pad & Left Joystick: Map the physical D-Pad (Up, Down, Left, Right) or the Left Analog Stick movements to the JOY1DAT (0xDFF00C) quadrature register bits, reusing the logic we established for the keyboard arrow keys.

2.2- Fire Button: Map the primary gamepad action button (e.g., SDL_GAMEPAD_BUTTON_SOUTH, which corresponds to 'A' on Xbox or 'Cross' on PlayStation) to control the Joystick 2 Fire Button on CIA-A Port A, bit 7 (0xBFE001) (0 for pressed, 1 for released).

Ensure all opened gamepad resources are cleanly closed when exiting the application. Let's integrate this feature to play commercial games with a real controller.

# Fix crash games

Some commercial games are crashing, hanging on a black screen, or failing to boot. Let's implement a robust Hardware Diagnostic and Crash Reporting System to help us identify what specific hardware feature or opcode behavior is missing or failing.

Trap Critical 68000 Exceptions:

Implement full detection and terminal warnings for Address Errors (attempting to read/write a 16-bit or 32-bit word at an odd memory address).

Trap Illegal Opcodes and print the exact hex values and the PC where it happened.

Monitor Double Bus Faults (when a critical exception occurs while the CPU is already processing an exception) and halt the emulation cleanly.

Implement an In-Memory Ring Buffer Log:

Create a small rolling history buffer that keeps track of the last 100 executed instructions (storing PC, opcode hex, and basic register state D0-D7/A0-A7).

When a crash occurs, print this execution history to the console so we can see the exact code pathway leading to the failure.

Monitor Hardware Deadlocks (Hang Detection):

If the CPU executes the exact same PC address or loops within a tiny 2-instruction window more than 50,000 times consecutively, flag a Deadlock Warning.

In this warning, display the current state of custom registers, specifically DMACON (DMA Status), INTREQ (Interrupt Requests), and the Blitter Busy bit (BBUSY in BLTCON0/DMACONR) to see if the game is waiting forever for an interrupt or a copper/blitter action.

Optional Fast RAM Toggle:

Provide an easy boolean toggle or architecture flag in MemoryBus to optionally expand the system memory with 512KB of Slow/Fast RAM mapped at 0xC00000 (the typical Amiga 500 trapdoor expansion), as many games require 1MB total RAM to boot.

Let's integrate this diagnostic engine so we can get precise logs when a game fails to start.

