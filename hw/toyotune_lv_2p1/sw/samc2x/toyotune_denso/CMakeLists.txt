cmake_minimum_required(VERSION 3.21)

# Toyotune v2.1 board firmware, ATSAMC21J18A (Cortex-M0+).
#
# This mirrors the Atmel Studio 7 project (toyotune_denso.cproj) so that the
# two build the same binary. If you change the source list, the compile flags
# or the linker script here, change them there too - or the Studio build and
# this one will quietly diverge.
#
# Requires, none of which need Atmel/Microchip Studio to be installed:
#   - the Arm GNU toolchain (arm-none-eabi-gcc)
#   - the SAMC21 DFP  ("Device Family Pack") - provides sam.h / samc21j18a.h
#   - the CMSIS pack  - provides the Cortex-M core headers
# Both packs are published as .atpack files, which are ordinary ZIPs; unzip
# them anywhere and point SAMC21_DFP_DIR / CMSIS_DIR at them.

project(toyotune_denso C ASM)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Which Denso CPU of the ECU this binary is for. See config.h - this is passed
# as a compile definition so one source tree can produce both binaries without
# editing config.h. Note CPU2 is deliberately an #error until its ROM image and
# DMA layout exist, so -DTOYOTUNE_CPU=2 is expected to fail the build for now.
set(TOYOTUNE_CPU "1" CACHE STRING "Target Denso CPU: 1 or 2")
set_property(CACHE TOYOTUNE_CPU PROPERTY STRINGS 1 2)

if(NOT TOYOTUNE_CPU MATCHES "^[12]$")
    message(FATAL_ERROR "TOYOTUNE_CPU must be 1 or 2, got '${TOYOTUNE_CPU}'")
endif()

# Which car. Both families share the inter-CPU frame geometry, so this mainly
# selects the ROM image served from image.c. See config.h.
set(TOYOTUNE_ECU "MR2" CACHE STRING "Target ECU family: MR2 or ST205")
set_property(CACHE TOYOTUNE_ECU PROPERTY STRINGS MR2 ST205)

if(NOT TOYOTUNE_ECU MATCHES "^(MR2|ST205)$")
    message(FATAL_ERROR "TOYOTUNE_ECU must be MR2 or ST205, got '${TOYOTUNE_ECU}'")
endif()

# Defaults point at the packs unzipped under sw/packs/ (see INSTALL.md). They
# are not committed - too large, and freely re-downloadable - so a fresh clone
# must unzip them before building, or override these to point elsewhere (an
# Atmel Studio install keeps them under
# C:/Program Files (x86)/Atmel/Studio/7.0/Packs/...).
set(PACKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../packs"
    CACHE PATH "Root of the unzipped device packs")
set(SAMC21_DFP_DIR "${PACKS_DIR}/SAMC21_DFP/samc21/include"
    CACHE PATH "SAMC21 Device Family Pack include directory (contains sam.h)")
set(CMSIS_DIR "${PACKS_DIR}/CMSIS/CMSIS/Core/Include"
    CACHE PATH "CMSIS Core include directory (contains core_cm0plus.h)")

foreach(_dir SAMC21_DFP_DIR CMSIS_DIR)
    if(NOT EXISTS "${${_dir}}")
        message(FATAL_ERROR
            "${_dir} does not exist:\n  ${${_dir}}\n"
            "Unzip the device packs (see INSTALL.md) and set "
            "-D${_dir}=<path>, or set -DPACKS_DIR=<packs root>.")
    endif()
endforeach()

set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../common")
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/Device_Startup/samc21j18a_flash.ld")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# Listed explicitly, deliberately: this directory also contains mode.c,
# toyotune_avr.c, sram.c and the esp* set, which the Studio project does NOT
# compile. Several of them include <avr/wdt.h>, <avr/sleep.h> and
# <util/atomic.h>, so a glob would pull in AVR sources and fail to build.
add_executable(${PROJECT_NAME}
    Device_Startup/startup_samc21.c
    Device_Startup/system_samc21.c
    can.c
    can_telemetry.c
    clk.c
    debug.c
    debug_printf.c
    diag.c
    diag_can.c
    dmac.c
    dmcu.c
    evsys.c
    image.c
    main.c
    os.c
    os_pendsv.s
    sdl.c
    sercom.c
    xmem.c
    ${COMMON_DIR}/mem.c
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "${PROJECT_NAME}"
    SUFFIX ".elf"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMMON_DIR}
    ${SAMC21_DFP_DIR}
    ${CMSIS_DIR}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    __SAMC21J18A__
    OS_CPU_ID
    TOYOTUNE_CPU${TOYOTUNE_CPU}
    TOYOTUNE_ECU_${TOYOTUNE_ECU}
    $<$<CONFIG:Debug>:DEBUG>
)

# -mlong-calls and -ffunction-sections (without -fdata-sections) match what the
# Studio project uses; keep them so both builds produce comparable output.
target_compile_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -ffunction-sections
    -mlong-calls
    -Wall
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-Os>
)

target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -T${LINKER_SCRIPT}
    -Wl,--gc-sections
    -Wl,-Map=${PROJECT_NAME}.map
    # If the link fails on undefined _sbrk/_write/_close and friends, newlib is
    # pulling in syscall stubs; add --specs=nosys.specs here.
)

target_link_libraries(${PROJECT_NAME} PRIVATE m)

# Relink when the linker script changes - CMake does not track -T as a dependency.
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

# ---------------------------------------------------------------------------
# The Denso ROM image (image.c)
# ---------------------------------------------------------------------------
#
# image.c is generated, not written: it is the 32K image the SAMC21 writes into
# the Denso MCU's SRAM at boot, built from one of the ROM sources under roms/.
# tools/build_image.py assembles that source, applies the OMODE patch and the
# 0xAA55 checksum the ECU's own power-on test expects, pads to 32K and emits
# the DMCU_Image[] array.
#
# It is deliberately NOT part of the default build: regenerating needs Python,
# and the image the firmware ships should change only when someone means it to.
# Two targets are offered instead:
#
#   cmake --build . --target rom-image           regenerate image.c
#   cmake --build . --target verify-rom-image    check it still matches its source
#
# TOYOTUNE_ROM_SOURCE follows TOYOTUNE_ECU/TOYOTUNE_CPU by default, so the
# image and the config.h selection cannot drift apart unnoticed. Note that
# config.h currently #errors on anything but MR2/CPU1 - the other sources
# assemble fine, but the firmware side is not finished for them.
#
# Where a ROM has a DIAG16 variant under its toyotune/ directory, that is the
# one to use: diag.c's live read/write link is not a hardware bus cycle but a
# cooperative protocol, so it only works if the code running on the Denso side
# implements it. The DIAG16 mod adds exactly that - an extended serial
# diagnostics block at 0x8000 handling the same 0xDA read-16 / 0xDB,0xDC
# write-16 / 0xDD,0xDE write-8 commands diag.c sends. Against a stock ROM
# diag.c has nothing to talk to.
set(_rom_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../roms/3S-GTE")
if(TOYOTUNE_ECU STREQUAL "MR2")
    if(TOYOTUNE_CPU STREQUAL "1")
        set(_rom_default "${_rom_root}/D151803-9651/toyotune/D151803-9651_DIAG16_32K.ASM")
    else()
        # No DIAG16 variant exists for -9661 yet, so this is the stock ROM and
        # diag.c will not work against it.
        set(_rom_default "${_rom_root}/D151803-9661/D151803-9661.ASM")
    endif()
else()
    if(TOYOTUNE_CPU STREQUAL "1")
        set(_rom_default "${_rom_root}/D151804-0461/toyotune/D151804-0461_DIAG16_32K_JS.ASM")
    else()
        # -0471's DIAG16 variant does not currently assemble (the D8X assembler
        # reports "did not converge after 10 passes"), so this stays stock.
        set(_rom_default "${_rom_root}/D151804-0471/D151804-0471.ASM")
    endif()
endif()

# Deliberately defaults to EMPTY rather than to _rom_default. A cache entry
# holding the resolved path would be sticky: it is written on the first
# configure and then never updated, so later changing TOYOTUNE_ECU/TOYOTUNE_CPU
# - or changing the default here - would silently keep generating the old ROM
# in an existing build directory. Empty means "follow the selection above".
set(TOYOTUNE_ROM_SOURCE "" CACHE FILEPATH
    "ROM source (.asm/.bin) image.c is generated from; empty follows TOYOTUNE_ECU/TOYOTUNE_CPU")

if(TOYOTUNE_ROM_SOURCE)
    set(_rom_source "${TOYOTUNE_ROM_SOURCE}")
else()
    set(_rom_source "${_rom_default}")
endif()

if(NOT EXISTS "${_rom_source}")
    message(WARNING "ROM source does not exist:\n  ${_rom_source}\n"
                    "The rom-image / verify-rom-image targets will fail.")
endif()

message(STATUS "ROM image source: ${_rom_source}")

find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
    set(_build_image "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_image.py")

    add_custom_target(rom-image
        COMMAND ${Python3_EXECUTABLE} "${_build_image}"
                "${_rom_source}"
                -o "${CMAKE_CURRENT_SOURCE_DIR}/image.c"
        COMMENT "Regenerating image.c from ${_rom_source}"
        VERBATIM
    )

    add_custom_target(verify-rom-image
        COMMAND ${Python3_EXECUTABLE} "${_build_image}"
                "${_rom_source}"
                --verify "${CMAKE_CURRENT_SOURCE_DIR}/image.c"
        COMMENT "Checking image.c matches ${_rom_source}"
        VERBATIM
    )
else()
    message(STATUS
        "Python not found - the rom-image / verify-rom-image targets are "
        "unavailable. The firmware still builds; image.c is committed.")
endif()

# ---------------------------------------------------------------------------
# Flashable images and listings, matching the Studio project's post-build step
# ---------------------------------------------------------------------------

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex -R .eeprom -R .fuse -R .lock -R .signature
            $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_COMMAND} -E echo "-- Generating ${PROJECT_NAME}.lss"
    COMMAND ${CMAKE_OBJDUMP} -h -S $<TARGET_FILE:${PROJECT_NAME}> > ${PROJECT_NAME}.lss
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Creating .bin/.hex/.lss and reporting size (${TOYOTUNE_ECU} CPU${TOYOTUNE_CPU})"
    VERBATIM
)
