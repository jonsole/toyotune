# Toolchain file for the bare-metal ARM Cortex-M0+ (ATSAMC21J18A) build.
#
# Point CMake at this with -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake,
# or just use one of the presets in CMakePresets.json which do it for you.
#
# The toolchain does not have to be on PATH: set ARM_TOOLCHAIN_DIR to the
# directory containing arm-none-eabi-gcc, either in the preset or on the
# command line.

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  arm)

# Without this CMake tries to build and *link* a test executable to verify the
# compiler works. There is no runtime to link against on bare metal, so that
# check fails and configuration aborts. Building a static library instead is
# the standard way round it.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARM_TOOLCHAIN_DIR "" CACHE PATH
    "Directory containing arm-none-eabi-gcc. Leave empty to search PATH.")

if(ARM_TOOLCHAIN_DIR)
    set(_tc_prefix "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-")
else()
    set(_tc_prefix "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER    "${_tc_prefix}gcc")
set(CMAKE_ASM_COMPILER  "${_tc_prefix}gcc")
set(CMAKE_OBJCOPY       "${_tc_prefix}objcopy" CACHE FILEPATH "objcopy")
set(CMAKE_OBJDUMP       "${_tc_prefix}objdump" CACHE FILEPATH "objdump")
set(CMAKE_SIZE          "${_tc_prefix}size"    CACHE FILEPATH "size")

# Only look for programs on the host; headers and libraries come from the
# toolchain and the device packs, never from the host system.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
