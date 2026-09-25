#include "test_framework.hpp"

int main() {
    run_timing_tests();
    run_memory_bus_tests();
    run_cpu68000_tests();
    run_display_tests();
    run_copper_tests();
    run_machine_tests();
    run_cpu_instruction_tests();
    run_cia_tests();
    run_cpu_isa_tests();
    run_floppy_tests();
    run_audio_tests();
    run_keyboard_joystick_tests();
    run_gamepad_mapping_tests();
    run_diagnostics_tests();
    run_kickstart_boot_test();

    if (test::g_failures == 0) {
        std::puts("All tests passed.");
        return 0;
    }
    std::fprintf(stderr, "%d check(s) failed.\n", test::g_failures);
    return 1;
}
