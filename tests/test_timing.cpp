#include <cstdint>

#include "core/timing.hpp"
#include "test_framework.hpp"

namespace {

using namespace amiga::timing;

void test_pal_constants() {
    EXPECT(kFrameDurationNs == 20'000'000ull);
    EXPECT(kPalColorClocksPerFrame == 71'051u);
    EXPECT(kCpuCyclesPerFrame == 142'102u);
    EXPECT(kPalColorClockHz == 3'546'895u);
}

void test_pacer_waits_until_deadline() {
    FramePacer p;
    p.reset(1'000);
    EXPECT(p.time_until_next_frame(1'000) == kFrameDurationNs);
    EXPECT(p.time_until_next_frame(1'000 + kFrameDurationNs) == 0);
}

void test_pacer_does_not_drift() {
    FramePacer p;
    p.reset(0);
    // Each frame finishes a little late; deadlines must stay on the 20 ms grid.
    for (uint64_t i = 1; i <= 1000; ++i) {
        p.frame_done(i * kFrameDurationNs + 300'000);
    }
    EXPECT(p.next_deadline_ns() == 1001 * kFrameDurationNs);
    EXPECT(p.resync_count() == 0);
}

void test_pacer_resyncs_after_long_stall() {
    FramePacer p;
    p.reset(0);
    const uint64_t stall = 2'000'000'000ull;  // 2 s, e.g. window drag
    p.frame_done(stall);
    EXPECT(p.resync_count() == 1);
    EXPECT(p.next_deadline_ns() == stall + kFrameDurationNs);
}

}  // namespace

void run_timing_tests() {
    test_pal_constants();
    test_pacer_waits_until_deadline();
    test_pacer_does_not_drift();
    test_pacer_resyncs_after_long_stall();
}
