#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include "core/chipset.hpp"
#include "core/cia.hpp"
#include "core/cpu68000.hpp"
#include "core/keyboard.hpp"
#include "core/memory_bus.hpp"

namespace amiga {

// One executed instruction, as recorded by the trace (state before it ran).
struct TraceEntry {
    uint32_t pc = 0;
    uint16_t opcode = 0;  // as fetched (code may be overwritten later)
    uint16_t sr = 0;
    std::array<uint32_t, 16> regs{};  // D0-D7, A0-A7
};

// The whole Amiga 500: bus, 68000 and custom chips on a shared timeline.
//
// Execution is instruction-aligned: the CPU runs one instruction, then the
// chipset catches up color clock by color clock (Copper, beam, display),
// so the two never drift apart by more than one instruction. The CPU sees
// Paula's interrupt level with one instruction of latency, as on hardware
// (IPL sampling): a request raised during an instruction is taken after the
// next one.
// The CIAs and the keyboard run on the E clock (one tick per 5 color clocks);
// CIA-A's TOD counts frames (VSYNC), CIA-B's counts lines (HSYNC).
// DF0 turns in step with the color clock and feeds the disk controller.
// Not modelled yet: DMA cycle stealing and the CPU's E-clock wait states on
// CIA accesses.
//
// Holds ~1.5MB inline (RAM, ROM, frame buffer): create it on the heap.
class Machine {
public:
    static constexpr unsigned kColorClocksPerEClock = 5;  // E = CPU clock / 10

    static constexpr size_t kTraceSize = 128;
    // Deadlock warning: the same instruction, or a 2-instruction loop, run this
    // many times in a row without making progress: D0-D7/A0-A7 identical on
    // every pass (so counting and copying loops, which change registers, are
    // not flagged). Instructions of interrupt handlers, at a higher interrupt
    // mask than the loop, neither count nor break it.
    static constexpr uint32_t kDeadlockIterations = 50'000;

    struct Deadlock {
        uint32_t pc_a = 0;  // the loop's instruction(s)
        uint32_t pc_b = 0;
        uint64_t frame = 0;
    };

    Machine() noexcept {
        bus_.attach_custom_chips(&chipset_);
        bus_.attach_cias(&cias_);
        // The RESET instruction pulses the reset line of the CIAs (the ROM
        // overlay comes back on), not of the CPU.
        cpu_.set_reset_handler([](void* self) { static_cast<Machine*>(self)->cias_.reset(); }, this);
    }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    void load_kickstart(const std::filesystem::path& path) { bus_.load_kickstart(path); }

    // Puts an ADF image in DF0 (throws if it can't be loaded). Into an empty
    // drive it goes in at once. A disk already there is ejected now and the
    // drive stays empty for kDiskSwapFrames before the new one goes in, as
    // when a person swaps disks: games and trackdisk.device detect a swap by
    // seeing the drive empty in between. Not for the emulation loop: it allocates.
    static constexpr unsigned kDiskSwapFrames = 150;  // 3 s
    void insert_disk(const std::filesystem::path& adf) { insert_disk(AdfImage::load(adf)); }
    void insert_disk(AdfImage image) {
        auto disk = std::make_unique<AdfImage>(std::move(image));
        if (!cias_.df0().disk_inserted() && !pending_disk_) {
            cias_.df0().insert(std::move(disk));
            return;
        }
        cias_.df0().eject();
        pending_disk_ = std::move(disk);
        swap_countdown_ = kDiskSwapFrames;
    }
    void eject_disk() noexcept {
        cias_.df0().eject();
        pending_disk_.reset();
    }
    // True while a swapped-in disk is waiting to go in.
    [[nodiscard]] bool disk_swap_pending() const noexcept { return pending_disk_ != nullptr; }

    // Floppy turbo (off by default): while a disk DMA read is running, the
    // drive delivers a word every color clock instead of every 32 us (~113x
    // faster, but not instant: see tick_chipset). It changes where the disk
    // is when the next read starts, which breaks some loaders (Oh No! More
    // Lemmings then syncs between a sector's two sync words about once per
    // 100 reads). Loading stays quick without it: the front end warps.
    void set_floppy_turbo(bool enabled) noexcept { floppy_turbo_ = enabled; }
    [[nodiscard]] bool floppy_turbo() const noexcept { return floppy_turbo_; }
    // The front end may run unthrottled while this is true: a disk transfer
    // ran within the last kWarpHoldFrames. Not merely "motor on": some games
    // (R-Type II) leave the motor running for good.
    static constexpr uint64_t kWarpHoldFrames = 150;  // 3 s: covers trackdisk's pauses between reads
    [[nodiscard]] bool disk_busy() const noexcept {
        return disk_read_seen_ && frame_count_ - last_disk_read_frame_ <= kWarpHoldFrames;
    }

    // Reset: CIAs reset (overlay on), keyboard sends its power-up codes, CPU
    // fetches SSP/PC from ROM and starts running.
    void reset();

    // --- Input ------------------------------------------------------------------
    // Raw Amiga keycode ($00-$67), pressed or released.
    void key_event(uint8_t amiga_keycode, bool down) noexcept { keyboard_.key(amiga_keycode, down); }
    // Mouse on port 0: motion in counter units, buttons as pressed/released.
    void mouse_move(int dx, int dy) noexcept { chipset_.denise().mouse_move(dx, dy); }
    // Digital joystick on port 1: directions to JOY1DAT, fire to CIA-A PA7.
    void joystick(bool up, bool down, bool left, bool right, bool fire) noexcept {
        chipset_.denise().set_joystick(up, down, left, right);
        cias_.set_joystick_fire(fire);
    }
    void mouse_buttons(bool left, bool right, bool middle) noexcept {
        cias_.set_left_button(left);
        chipset_.paula().set_mouse_buttons(right, middle);
    }

    // Runs until the beam completes one frame (313 lines).
    void run_frame();

    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] Cpu68000& cpu() noexcept { return cpu_; }
    [[nodiscard]] Chipset& chipset() noexcept { return chipset_; }
    [[nodiscard]] Cias& cias() noexcept { return cias_; }
    [[nodiscard]] Keyboard& keyboard() noexcept { return keyboard_; }

    // --- Audio ------------------------------------------------------------------
    // 16-bit stereo (interleaved L, R) produced during the last run_frame(),
    // one sample per kColorClocksPerSample color clocks (~47.9 kHz of Amiga time).
    static constexpr unsigned kColorClocksPerSample = 74;
    static constexpr size_t kMaxAudioFrames = 1024;  // per video frame (960 needed)
    [[nodiscard]] std::span<const int16_t> audio() const noexcept {
        return std::span<const int16_t>(audio_buffer_).first(audio_frames_ * 2);
    }

    // --- Debugging ---------------------------------------------------------------
    // Records the state before each instruction in a ring buffer (on by
    // default: it costs a few percent).
    void set_trace_enabled(bool enabled) noexcept { trace_enabled_ = enabled; }
    [[nodiscard]] bool trace_enabled() const noexcept { return trace_enabled_; }
    // i = 0 is the most recent instruction; nullptr past what was recorded.
    [[nodiscard]] const TraceEntry* trace(size_t i) const noexcept {
        if (i >= trace_count_ || i >= kTraceSize) return nullptr;
        return &trace_[(trace_head_ + kTraceSize - 1 - i) % kTraceSize];
    }
    // Lowest/highest PC executed during the last frame (loop detection).
    [[nodiscard]] uint32_t frame_min_pc() const noexcept { return frame_min_pc_; }
    [[nodiscard]] uint32_t frame_max_pc() const noexcept { return frame_max_pc_; }
    [[nodiscard]] const char* halt_reason() const noexcept { return halt_reason_.data(); }
    // A deadlock detected since the last call (reported once per loop).
    [[nodiscard]] std::optional<Deadlock> take_deadlock() noexcept {
        auto result = deadlock_;
        deadlock_.reset();
        return result;
    }

    // Trapdoor slow RAM at $C00000 (on by default): off gives a stock 512KB A500.
    void set_slow_ram(bool enabled) noexcept { bus_.set_slow_ram(enabled); }

    [[nodiscard]] bool cpu_halted() const noexcept { return cpu_halted_; }
    [[nodiscard]] uint64_t cpu_cycles() const noexcept { return cpu_cycles_; }
    [[nodiscard]] uint64_t frame_count() const noexcept { return frame_count_; }

private:
    void tick_chipset();
    void tick_e_clock();
    void feed_disk(const FloppyDrive::Tick& disk);

    MemoryBus bus_;
    Chipset chipset_{bus_};
    Cias cias_{bus_};
    Keyboard keyboard_;
    Cpu68000 cpu_{bus_};
    unsigned e_clock_divider_ = 0;
    std::unique_ptr<AdfImage> pending_disk_;
    uint64_t last_disk_read_frame_ = 0;
    bool disk_read_seen_ = false;
    unsigned swap_countdown_ = 0;
    uint8_t sampled_ipl_ = 0;  // Paula's level at the start of the previous instruction
    std::array<int16_t, kMaxAudioFrames * 2> audio_buffer_{};
    size_t audio_frames_ = 0;
    int32_t audio_sum_left_ = 0;
    int32_t audio_sum_right_ = 0;
    unsigned audio_clocks_ = 0;
    bool floppy_turbo_ = false;

    // Both clocks count CPU cycles (1 color clock = 2 CPU cycles).
    uint64_t cpu_time_ = 0;
    uint64_t chip_time_ = 0;
    uint64_t cpu_cycles_ = 0;
    uint64_t frame_count_ = 0;
    bool cpu_halted_ = true;  // until reset()

    void check_deadlock(uint32_t pc) noexcept;

    bool trace_enabled_ = true;
    // Deadlock detection.
    std::array<uint32_t, 2> loop_history_{};  // previous two PCs
    uint32_t loop_count_ = 0;
    uint8_t loop_mask_ = 0;
    uint32_t loop_ignored_ = 0;  // instructions skipped as an interrupt handler
    static constexpr uint32_t kMaxHandlerInstructions = 20'000;
    uint32_t loop_head_ = 0xFFFF'FFFF;        // PC where passes are compared
    std::array<uint32_t, 16> loop_regs_{};  // registers at the previous pass
    bool loop_reported_ = false;
    std::optional<Deadlock> deadlock_;
    std::array<TraceEntry, kTraceSize> trace_{};
    size_t trace_head_ = 0;
    size_t trace_count_ = 0;
    uint32_t frame_min_pc_ = 0;
    uint32_t frame_max_pc_ = 0;
    std::array<char, 128> halt_reason_{};
};

}  // namespace amiga
