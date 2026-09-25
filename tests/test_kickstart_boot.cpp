#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "core/adf.hpp"
#include "core/denise.hpp"
#include "core/machine.hpp"
#include "test_framework.hpp"

amiga::AdfImage make_green_boot_disk();  // test_floppy.cpp

// Milestone integration test: boot a real Kickstart 1.3 (34.005) ROM until
// the "Insert Disk" screen is drawn. Kickstart is copyrighted and not part of
// the repository: set AMIGA_KICKSTART_13 to the ROM path to run it.
void run_kickstart_boot_test() {
    const char* rom = std::getenv("AMIGA_KICKSTART_13");
    if (rom == nullptr) {
        std::puts("kickstart boot test skipped (set AMIGA_KICKSTART_13=/path/to/kick34005.rom)");
        return;
    }
    auto machine = std::make_unique<amiga::Machine>();
    machine->load_kickstart(rom);
    machine->reset();
    for (int frame = 0; frame < 300 && !machine->cpu_halted(); ++frame) machine->run_frame();  // 6 s
    EXPECT(!machine->cpu_halted());

    // The Insert Disk picture: white background, blue disk ($77C), grey shutter ($BBB), black outline.
    const amiga::Denise& denise = machine->chipset().denise();
    EXPECT(denise.color(0) == 0x0FFF && denise.color(1) == 0x0000);
    EXPECT(denise.color(2) == 0x077C && denise.color(3) == 0x0BBB);
    unsigned blue = 0, grey = 0, black = 0;
    for (const uint32_t pixel : machine->chipset().frame()) {
        blue += pixel == amiga::Denise::rgb12_to_argb(0x077C);
        grey += pixel == amiga::Denise::rgb12_to_argb(0x0BBB);
        black += pixel == amiga::Denise::rgb12_to_argb(0x0000);
    }
    EXPECT(blue > 6'000 && grey > 1'500 && black > 4'000);  // 640x256 frame
    std::printf("kickstart boot test: Insert Disk screen after %llu frames (%u blue, %u grey, %u black pixels)\n",
                static_cast<unsigned long long>(machine->frame_count()), blue, grey, black);

    // Boot from disk: trackdisk.device reads the boot block through the
    // emulated drive and disk DMA, the strap checks it and runs it.
    auto booted = std::make_unique<amiga::Machine>();
    booted->load_kickstart(rom);
    booted->insert_disk(make_green_boot_disk());
    booted->reset();
    unsigned frames = 0;
    while (frames < 600 && booted->chipset().denise().color(0) != 0x00F0 && !booted->cpu_halted()) {
        booted->run_frame();
        ++frames;
    }
    booted->run_frame();
    EXPECT(booted->chipset().denise().color(0) == 0x00F0);
    EXPECT(booted->chipset().frame()[100 * 640 + 320] == amiga::Denise::rgb12_to_argb(0x00F0));
    std::printf("kickstart boot test: boot block from disk ran after %u frames\n", frames);
}
