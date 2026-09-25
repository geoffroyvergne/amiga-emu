#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include "core/chipset.hpp"
#include "core/cia.hpp"
#include "core/cpu68000.hpp"
#include "core/keyboard.hpp"
#include "core/memory_bus.hpp"

namespace amiga {

// One executed instruction, as recorded by the trace (state before it ran).
struct TraceEntry {
    uint32_t pc = 0;
    uint16_t sr = 0;
    std::array<uint32_t, 16> regs{};  // D0-D7, A0-A7
};

// The whole Amiga 500: bus, 68000 and custom chips on a shared timeline.
//
// Execution is instruction-aligned: the CPU runs one instruction, then the
// chipset catches up color clock by color clock (Copper, beam, display),
// so the two never drift apart by more than one instruction. The CPU samples
// Paula's interrupt level before each instruction.
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

    // Puts an ADF image in DF0 (replacing any disk; throws if it can't be loaded).
    // Not for the emulation loop: it allocates.
    void insert_disk(const std::filesystem::path& adf) {
        auto image = std::make_unique<AdfImage>(AdfImage::load(adf));
        cias_.df0().eject();
        cias_.df0().insert(std::move(image));
    }
    void insert_disk(AdfImage image) {
        cias_.df0().eject();
        cias_.df0().insert(std::make_unique<AdfImage>(std::move(image)));
    }
    void eject_disk() noexcept { cias_.df0().eject(); }

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

    // --- Debugging ---------------------------------------------------------------
    // Records the state before each instruction in a ring buffer (off by default).
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

    [[nodiscard]] bool cpu_halted() const noexcept { return cpu_halted_; }
    [[nodiscard]] uint64_t cpu_cycles() const noexcept { return cpu_cycles_; }
    [[nodiscard]] uint64_t frame_count() const noexcept { return frame_count_; }

private:
    void tick_chipset();
    void tick_e_clock();

    MemoryBus bus_;
    Chipset chipset_{bus_};
    Cias cias_{bus_};
    Keyboard keyboard_;
    Cpu68000 cpu_{bus_};
    unsigned e_clock_divider_ = 0;

    // Both clocks count CPU cycles (1 color clock = 2 CPU cycles).
    uint64_t cpu_time_ = 0;
    uint64_t chip_time_ = 0;
    uint64_t cpu_cycles_ = 0;
    uint64_t frame_count_ = 0;
    bool cpu_halted_ = true;  // until reset()

    bool trace_enabled_ = false;
    std::array<TraceEntry, kTraceSize> trace_{};
    size_t trace_head_ = 0;
    size_t trace_count_ = 0;
    uint32_t frame_min_pc_ = 0;
    uint32_t frame_max_pc_ = 0;
    std::array<char, 128> halt_reason_{};
};

}  // namespace amiga
