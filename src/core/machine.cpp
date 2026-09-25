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
    sampled_ipl_ = 0;
    cpu_halted_ = false;
    std::fprintf(stderr, "[machine] CPU reset: SSP=$%08X PC=$%08X\n", cpu_.a(7), cpu_.pc());
}

void Machine::run_frame() {
    ++frame_count_;
    audio_frames_ = 0;
    frame_min_pc_ = 0xFFFF'FFFF;
    frame_max_pc_ = 0;
    for (;;) {
        if (!cpu_halted_ && cpu_time_ <= chip_time_) {
            // Interrupt latency: the CPU acts on the level Paula had at the start
            // of the previous instruction, so a request raised during one
            // instruction is taken after the next. Code polling INTREQR while
            // the OS handler is enabled relies on seeing the bit in between.
            cpu_.set_interrupt_level(sampled_ipl_);
            sampled_ipl_ = chipset_.interrupt_level();
            const uint32_t pc = cpu_.pc();
            if (pc < frame_min_pc_) frame_min_pc_ = pc;
            if (pc > frame_max_pc_) frame_max_pc_ = pc;
            if (!cpu_.stopped()) check_deadlock(pc);
            if (trace_enabled_ && !cpu_.stopped()) {
                TraceEntry& entry = trace_[trace_head_];
                entry.pc = pc;
                entry.opcode = bus_.peek16(pc).value_or(0);
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
    if (pending_disk_ && --swap_countdown_ == 0) cias_.df0().insert(std::move(pending_disk_));
}

void Machine::check_deadlock(uint32_t pc) noexcept {
    const uint8_t mask = cpu_.interrupt_mask();
    // At a higher interrupt mask than the loop: an interrupt handler, ignored
    // (it will return to the loop). Unless it goes on for too long: then the
    // program itself raised its mask (e.g. MOVE #$2700,SR) and moved on.
    if (loop_count_ != 0 && mask > loop_mask_) {
        if (++loop_ignored_ < kMaxHandlerInstructions) return;
        loop_count_ = 0;
        loop_head_ = 0xFFFF'FFFF;
        loop_mask_ = mask;
    }
    loop_ignored_ = 0;
    const bool looping = pc == loop_history_[0] || pc == loop_history_[1];
    if (!looping) {
        loop_count_ = 0;
        loop_head_ = 0xFFFF'FFFF;
        loop_reported_ = false;
        loop_mask_ = mask;
    } else if (pc != loop_head_ && loop_count_ == 0) {  // first repeat: this PC marks each pass
        loop_head_ = pc;
        for (unsigned r = 0; r < 8; ++r) {
            loop_regs_[r] = cpu_.d(r);
            loop_regs_[8 + r] = cpu_.a(r);
        }
        loop_count_ = 1;
    } else if (pc == loop_head_) {  // one more pass: stuck only if nothing changed
        bool same = true;
        for (unsigned r = 0; r < 8 && same; ++r) same = loop_regs_[r] == cpu_.d(r) && loop_regs_[8 + r] == cpu_.a(r);
        if (!same) {
            for (unsigned r = 0; r < 8; ++r) {
                loop_regs_[r] = cpu_.d(r);
                loop_regs_[8 + r] = cpu_.a(r);
            }
            loop_count_ = 1;
        } else if (++loop_count_ == kDeadlockIterations && !loop_reported_) {
            loop_reported_ = true;
            deadlock_ = Deadlock{pc, pc == loop_history_[0] ? loop_history_[1] : loop_history_[0], frame_count_};
        }
    }
    loop_history_[1] = loop_history_[0];
    loop_history_[0] = pc;
}

// Everything clocked alongside the custom chips, after each color clock.
void Machine::tick_chipset() {
    const Agnus& agnus = chipset_.agnus();

    // Audio: average Paula's output over each sample period (a box filter
    // against aliasing), then scale the two-channel sum (+-16384) to 16 bits.
    if (!chipset_.audio().idle()) {
        audio_sum_left_ += chipset_.audio().left();
        audio_sum_right_ += chipset_.audio().right();
    }
    if (++audio_clocks_ == kColorClocksPerSample) {
        if (audio_frames_ < kMaxAudioFrames) {
            const auto scale = [](int32_t sum) {
                const int32_t value = sum * 2 / static_cast<int32_t>(kColorClocksPerSample);
                return static_cast<int16_t>(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
            };
            audio_buffer_[audio_frames_ * 2] = scale(audio_sum_left_);
            audio_buffer_[audio_frames_ * 2 + 1] = scale(audio_sum_right_);
            ++audio_frames_;
        }
        audio_sum_left_ = audio_sum_right_ = 0;
        audio_clocks_ = 0;
    }
    if (agnus.hpos() == 0) {  // a new line started (HSYNC), maybe a new frame (VSYNC)
        cias_.b().tod_pulse();
        if (agnus.vpos() == 0) cias_.a().tod_pulse();
    }
    feed_disk(cias_.df0().tick());
    // Turbo: one extra word per color clock while a transfer runs (~113x the
    // real speed). Not instant on purpose: loaders commonly start the DMA and
    // only then clear DSKBLK in INTREQ, so the transfer must still take a
    // little while (a track is ~13000 CPU cycles).
    if (chipset_.disk().active() && agnus.dma_enabled(reg::kDskEn)) {
        last_disk_read_frame_ = frame_count_;
        disk_read_seen_ = true;
        if (floppy_turbo_) feed_disk(cias_.df0().next_word());
    }
    if (++e_clock_divider_ == kColorClocksPerEClock) {
        e_clock_divider_ = 0;
        tick_e_clock();
    }
}

void Machine::feed_disk(const FloppyDrive::Tick& disk) {
    if (disk.index) cias_.b().flag_pulse();
    if (disk.word_ready) chipset_.disk_word(disk.word);
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
