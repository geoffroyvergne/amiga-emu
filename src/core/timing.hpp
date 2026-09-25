#pragma once

#include <cstdint>

namespace amiga::timing {

// PAL Amiga 500 clocks (Amiga Hardware Reference Manual, 3rd ed.).
inline constexpr uint32_t kPalCpuClockHz = 7'093'790;  // 68000 clock
inline constexpr uint32_t kPalColorClockHz = kPalCpuClockHz / 2;  // 3.546895 MHz

// PAL beam: 313 lines (non-interlaced, long frames) of 227 color clocks
// (hpos $00-$E2). One color clock = 2 CPU cycles.
inline constexpr uint32_t kPalColorClocksPerLine = 227;
inline constexpr uint32_t kPalLinesPerFrame = 313;
inline constexpr uint32_t kPalColorClocksPerFrame = kPalColorClocksPerLine * kPalLinesPerFrame;
inline constexpr uint32_t kCpuCyclesPerColorClock = 2;
inline constexpr uint32_t kCpuCyclesPerFrame = kPalColorClocksPerFrame * kCpuCyclesPerColorClock;  // 142'102

// Host frame pacing: 50 Hz => 20 ms per frame. An emulated frame is exactly
// one beam frame (~19.97 ms of Amiga time, i.e. 49.92 Hz), so the emulation
// runs 0.16% slower than a real PAL Amiga in wall-clock time.
inline constexpr uint32_t kPalFrameRateHz = 50;
inline constexpr uint64_t kFrameDurationNs = 1'000'000'000ull / kPalFrameRateHz;

static_assert(kFrameDurationNs == 20'000'000ull);

// Deadline-based frame pacer. Uses absolute deadlines (not "sleep 20 ms after
// each frame") so rounding errors and render time don't accumulate into drift.
// Pure logic, no host calls: the caller supplies the current time in ns.
class FramePacer {
public:
    // Frames further behind than this are dropped rather than caught up,
    // e.g. after the window was dragged or the process was suspended.
    static constexpr uint64_t kMaxLagNs = kFrameDurationNs * 5;

    void reset(uint64_t now_ns) noexcept { next_deadline_ns_ = now_ns + kFrameDurationNs; }

    // Nanoseconds to wait before the next frame may start (0 = run now).
    [[nodiscard]] uint64_t time_until_next_frame(uint64_t now_ns) const noexcept {
        return now_ns >= next_deadline_ns_ ? 0 : next_deadline_ns_ - now_ns;
    }

    // Call once a frame has been emitted. Advances the deadline by exactly one
    // frame period, or resynchronises if we have fallen too far behind.
    void frame_done(uint64_t now_ns) noexcept {
        next_deadline_ns_ += kFrameDurationNs;
        if (now_ns > next_deadline_ns_ + kMaxLagNs) {
            next_deadline_ns_ = now_ns + kFrameDurationNs;
            ++resyncs_;
        }
    }

    [[nodiscard]] uint64_t next_deadline_ns() const noexcept { return next_deadline_ns_; }
    [[nodiscard]] uint64_t resync_count() const noexcept { return resyncs_; }

private:
    uint64_t next_deadline_ns_ = 0;
    uint64_t resyncs_ = 0;
};

}  // namespace amiga::timing
