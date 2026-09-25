# nbajam_ofe - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nbajam_ofe LANGUAGES CXX)

# RC is only needed to carry the executable's icon (res/app.rc).
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
option(REXPORT_CRASH_REPORT "Resolve guest call stacks on access violations" ON)
option(REXPORT_GUEST_PROFILER "Sampling profiler for locating guest code" ON)

set(NBAJAMOFE_SOURCES
    src/main.cpp
    src/kernel_usbcam_stubs.cpp
    src/nbajam_hooks.cpp
    src/nbajam_probes.cpp
    src/vp6_hooks.cpp
    src/dds_textures.cpp
    src/scripted_input.cpp
    src/apt_trace.cpp
    src/mod_saves.cpp
    src/mod_swap.cpp
    src/mod_picker.cpp
    src/host_pad.cpp
    src/window_title.cpp
)

if(WIN32)
    list(APPEND NBAJAMOFE_SOURCES res/app.rc)
endif()

if(REXPORT_CRASH_REPORT AND WIN32)
    list(APPEND NBAJAMOFE_SOURCES src/crash_report.cpp)
endif()

# Inert unless NBAJAM_PROFILE_SECONDS is set in the environment.
if(REXPORT_GUEST_PROFILER AND WIN32)
    list(APPEND NBAJAMOFE_SOURCES src/guest_profiler.cpp)
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

# ReXGlue v0.10.0 translates vpkuwus/vpkuhus by writing the destination
# register one element at a time while still reading the sources, which is
# wrong whenever the destination is also a source. All three vpkuwus128 in
# this image are in the VP6 video decoder and all three alias, which is what
# made the EA Sports intro play with green corruption.
#
# This rewrites those instructions after codegen and before they are compiled.
# It is idempotent and leaves files alone when there is nothing to change, so
# it does not dirty the build. See tools/fix_vector_aliasing.py and BUILDING.md.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(nbajam_ofe_fix_vectors
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/fix_vector_aliasing.py" --apply
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Repairing vector instructions whose destination is also a source"
    VERBATIM)
add_dependencies(nbajam_ofe_fix_vectors nbajam_ofe_codegen)
add_dependencies(nbajam_ofe_recomp nbajam_ofe_fix_vectors)
