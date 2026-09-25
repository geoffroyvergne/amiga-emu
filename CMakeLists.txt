cmake_minimum_required(VERSION 3.22)

project(amiga_emu
    VERSION 0.1.0
    DESCRIPTION "Amiga 500 (OCS) emulator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# --- SDL3 --------------------------------------------------------------------
find_package(SDL3 CONFIG REQUIRED)

# --- Common warnings ---------------------------------------------------------
add_library(amiga_warnings INTERFACE)
if(MSVC)
    target_compile_options(amiga_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(amiga_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

# --- Emulator core (SDL-free, testable) -------------------------------------
add_library(amiga_core STATIC
    src/core/memory_bus.cpp
    src/core/cpu68000.cpp
    src/core/copper.cpp
    src/core/blitter.cpp
    src/core/cia.cpp
    src/core/adf.cpp
    src/core/denise.cpp
    src/core/chipset.cpp
    src/core/machine.cpp
    src/core/disassembler.cpp
    src/core/debug_dump.cpp)
target_include_directories(amiga_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(amiga_core PUBLIC amiga_warnings)

# --- Frontend executable -----------------------------------------------------
add_executable(amiga_emu src/main.cpp)
target_link_libraries(amiga_emu PRIVATE amiga_core SDL3::SDL3)

# --- Tests -------------------------------------------------------------------
add_executable(tests_emu
    tests/test_main.cpp
    tests/test_timing.cpp
    tests/test_memory_bus.cpp
    tests/test_cpu68000.cpp
    tests/test_display.cpp
    tests/test_copper.cpp
    tests/test_machine.cpp
    tests/test_cpu_instructions.cpp
    tests/test_cia.cpp
    tests/test_cpu_isa.cpp
    tests/test_kickstart_boot.cpp
    tests/test_floppy.cpp)
target_link_libraries(tests_emu PRIVATE amiga_core)

enable_testing()
add_test(NAME tests_emu COMMAND tests_emu)
