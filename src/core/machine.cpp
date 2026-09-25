#include "core/machine.hpp"

#include <cstdio>
#include <exception>

#include "core/timing.hpp"

namespace amiga {

void Machine::reset() {
    bus_.reset();
    cias_.reset();
    keyboard_.reset();
    cpu_.reset();
    cpu_time_ = chip_time_;
    cpu_halted_ = false;
    std::fprintf(stderr, "[machine] CPU reset: SSP=$%08X PC=$%08X\n", cpu_.a(7), cpu_.pc());
}

void Machine::run_frame() {
    ++frame_count_;
    frame_min_pc_ = 0xFFFF'FFFF;
    frame_max_pc_ = 0;
    for (;;) {
        if (!cpu_halted_ && cpu_time_ <= chip_time_) {
            cpu_.set_interrupt_level(chipset_.interrupt_level());
            const uint32_t pc = cpu_.pc();
            if (pc < frame_min_pc_) frame_min_pc_ = pc;
            if (pc > frame_max_pc_) frame_max_pc_ = pc;
            if (trace_enabled_ && !cpu_.stopped()) {
                TraceEntry& entry = trace_[trace_head_];
                entry.pc = pc;
                entry.sr = cpu_.sr();
                for (unsigned r = 0; r < 8; ++r) {
                    entry.regs[r] = cpu_.d(r);
                    entry.regs[8 + r] = cpu_.a(r);
                }
                trace_head_ = (trace_head_ + 1) % kTraceSize;
                ++trace_count_;
            }
            try {
                const uint32_t cycles = cpu_.step();
                cpu_time_ += cycles;
                cpu_cycles_ += cycles;
            } catch (const std::exception& e) {
                // Panic: something isn't emulated yet. Freeze the CPU; the
                // chipset keeps running so the display stays alive.
                std::fprintf(stderr, "[machine] CPU halted: %s\n", e.what());
                std::snprintf(halt_reason_.data(), halt_reason_.size(), "%s", e.what());
                cpu_halted_ = true;
            }
            continue;
        }

        const bool frame_done = chipset_.tick();
        chip_time_ += timing::kCpuCyclesPerColorClock;
        tick_chipset();
        if (frame_done) break;
    }
    if (cpu_halted_) cpu_time_ = chip_time_;
}

// Everything clocked alongside the custom chips, after each color clock.
void Machine::tick_chipset() {
    const Agnus& agnus = chipset_.agnus();
    if (agnus.hpos() == 0) {  // a new line started (HSYNC), maybe a new frame (VSYNC)
        cias_.b().tod_pulse();
        if (agnus.vpos() == 0) cias_.a().tod_pulse();
    }
    const FloppyDrive::Tick disk = cias_.df0().tick();
    if (disk.index) cias_.b().flag_pulse();
    if (disk.word_ready) chipset_.disk_word(disk.word);
    if (++e_clock_divider_ == kColorClocksPerEClock) {
        e_clock_divider_ = 0;
        tick_e_clock();
    }
}

void Machine::tick_e_clock() {
    cias_.a().tick();
    cias_.b().tick();
    keyboard_.tick(cias_.a());
    // /IRQ lines are level-triggered: INTREQ keeps being set until the ICR is read.
    if (cias_.a().irq()) chipset_.paula().request(reg::kIntPorts);
    if (cias_.b().irq()) chipset_.paula().request(reg::kIntExter);
}

}  // namespace amiga
