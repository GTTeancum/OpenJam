# nbajam_ofe - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nbajam_ofe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
option(REXPORT_CRASH_REPORT "Resolve guest call stacks on access violations" ON)

set(NBAJAMOFE_SOURCES
    src/main.cpp
    src/kernel_usbcam_stubs.cpp
    src/nbajam_hooks.cpp
    src/nbajam_probes.cpp
)

if(REXPORT_CRASH_REPORT AND WIN32)
    list(APPEND NBAJAMOFE_SOURCES src/crash_report.cpp)
endif()

if(WIN32)
    add_executable(nbajam_ofe WIN32 ${NBAJAMOFE_SOURCES})
else()
    add_executable(nbajam_ofe ${NBAJAMOFE_SOURCES})
endif()

# GPU_PLUGINS is required: the Xenos backend is loaded at runtime rather than
# linked, so nothing stages rexgpu-xenos*.dll beside the executable without
# it and the game comes up with no graphics backend. `rexglue init` does not
# emit this.
rexglue_setup_target(nbajam_ofe GPU_PLUGINS xenos)
