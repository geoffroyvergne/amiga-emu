#pragma once

#include <cstdio>

// Minimal self-contained test helpers (no external framework).
namespace test {

inline int g_failures = 0;

}  // namespace test

#define EXPECT(cond)                                                                 \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond);  \
            ++test::g_failures;                                                      \
        }                                                                            \
    } while (0)

#define EXPECT_THROWS(expr, exception_type)                                          \
    do {                                                                             \
        bool caught_ = false;                                                        \
        try {                                                                        \
            (void)(expr);                                                            \
        } catch (const exception_type&) {                                            \
            caught_ = true;                                                          \
        }                                                                            \
        if (!caught_) {                                                              \
            std::fprintf(stderr, "%s:%d: FAILED: %s did not throw %s\n", __FILE__,   \
                         __LINE__, #expr, #exception_type);                          \
            ++test::g_failures;                                                      \
        }                                                                            \
    } while (0)

void run_timing_tests();
void run_memory_bus_tests();
void run_cpu68000_tests();
void run_display_tests();
void run_copper_tests();
void run_machine_tests();
void run_cpu_instruction_tests();
void run_cia_tests();
void run_cpu_isa_tests();
void run_kickstart_boot_test();
void run_floppy_tests();
void run_audio_tests();
void run_keyboard_joystick_tests();
void run_gamepad_mapping_tests();
void run_diagnostics_tests();
