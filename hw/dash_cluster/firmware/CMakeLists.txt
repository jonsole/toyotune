# Dash cluster node firmware - RP2350.
#
# Target board is the Waveshare RP2350-Touch-AMOLED-1.75: an RP2350A, so
# GPIO0..29 and every pin inside PIO's 0..31 window. See hw/dash_cluster/PLAN.md
# for the pin allocation and why identity is a resistor divider rather than a
# build-time define.
#
# Build:
#   cmake -B build -G Ninja
#   cmake --build build
#
# The SDK is found from PICO_SDK_PATH, or from the VS Code extension's install
# under ~/.pico-sdk if that is not set - see below.

cmake_minimum_required(VERSION 3.13)

# The Pico VS Code extension installs the SDK, toolchain and ninja under
# ~/.pico-sdk rather than on PATH, so a plain shell build finds nothing without
# this. Prefer an explicit PICO_SDK_PATH when one is set.
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(_vscode_sdk "$ENV{USERPROFILE}/.pico-sdk/sdk/2.2.0")
    if(NOT EXISTS "${_vscode_sdk}")
        set(_vscode_sdk "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
    endif()
    if(EXISTS "${_vscode_sdk}")
        set(PICO_SDK_PATH "${_vscode_sdk}" CACHE PATH "Pico SDK")
        message(STATUS "Using the VS Code extension's SDK: ${PICO_SDK_PATH}")
    endif()
endif()

set(PICO_PLATFORM rp2350 CACHE STRING "RP2350, not RP2040")
set(PICO_BOARD pico2 CACHE STRING "Closest stock board definition")

# RP2350 needs picotool to produce the UF2, and the SDK will otherwise build it
# from source - which needs a HOST compiler, not the ARM one, and so fails on a
# machine where only arm-none-eabi is on PATH. The VS Code extension ships a
# prebuilt picotool; point at it and the source build never happens.
if(NOT picotool_DIR)
    foreach(_root "$ENV{USERPROFILE}/.pico-sdk" "$ENV{HOME}/.pico-sdk")
        file(GLOB _picotool_dirs "${_root}/picotool/*/picotool")
        foreach(_d ${_picotool_dirs})
            if(EXISTS "${_d}/picotoolConfig.cmake")
                set(picotool_DIR "${_d}" CACHE PATH "Prebuilt picotool")
                message(STATUS "Using prebuilt picotool: ${picotool_DIR}")
                break()
            endif()
        endforeach()
    endforeach()
endif()

include(pico_sdk_import.cmake)

project(dash_node C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

# ---------------------------------------------------------------------------
# can2040 - software CAN on a PIO block
# ---------------------------------------------------------------------------
#
# Not vendored: it is a third-party library with its own history, and pinning a
# copy here would hide which revision is in use. Clone it beside this tree or
# point CAN2040_PATH at it:
#
#   git clone https://github.com/KevinOConnor/can2040
#
# The firmware builds without it so the parts that do not need a bus - decode,
# the store, the page tables - can be worked on and tested meanwhile. It will
# say so on the console rather than silently receiving nothing.
set(CAN2040_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/can2040"
    CACHE PATH "Path to a can2040 checkout")

set(DASH_SOURCES
    src/main.c
    src/node_id.c
    src/pages.c
    src/signal_store.c
    src/signals.c
    src/telemetry.c
    src/ui_model.c
)

# ---------------------------------------------------------------------------
# The display
# ---------------------------------------------------------------------------
#
# LVGL IS NOT PART OF THIS BUILD, AND THAT IS DELIBERATE.
#
# The dials are pre-rendered pictures and what is drawn live is a needle and a
# number, so the toolkit was doing very little at a real cost - see
# hw/dash_cluster/RENDERER_PLAN.md for the measurements. The renderer is now
# src/ui_draw.c: an 8-bit paletted back buffer, converted to RGB565 on its way
# to the panel by src/panel.c.
#
# LVGL is still used to DESIGN the dials, on the PC, by tools/face_render -
# which is where lv_conf.h and src/ui_gauge.c now belong. Neither is compiled
# into the firmware. That tool needs an LVGL checkout; this does not.
#
# ui_model.c does not depend on the display at all and is always built, which
# is the point of splitting it out: the decisions a gauge makes are testable on
# a host whether or not there is a panel.

# Waveshare's CO5300 panel driver, its PIO-QSPI transport and the CST9217 touch
# driver, vendored from their demo zip - see vendor/README.md for provenance,
# the licence, and the three bugs found while reading it. Kept as delivered, so
# the corrections live in src/panel.c rather than in these files.
#
# Deliberately NOT compiled with the warning set below: they are someone else's
# code, and a warning nobody is going to fix is noise that hides the ones that
# matter.
set(PANEL_VENDOR_SOURCES
    vendor/AMOLED_1in75.c
    vendor/CST9217.c
    vendor/DEV_Config.c
    vendor/qspi_pio.c
)

option(DASH_HAVE_PANEL "Build the display layer" ON)
if(DASH_HAVE_PANEL)
    list(APPEND DASH_SOURCES src/panel.c src/ui_draw.c src/dash_faces.c)
    list(APPEND VENDOR_SOURCES ${PANEL_VENDOR_SOURCES})
    set(DASH_HAVE_PANEL 1)
else()
    message(STATUS "DASH_HAVE_PANEL is OFF - core 1 will report over USB "
                   "serial instead of drawing.")
    set(DASH_HAVE_PANEL 0)
endif()

if(EXISTS "${CAN2040_PATH}/src/can2040.c")
    message(STATUS "can2040: ${CAN2040_PATH}")
    list(APPEND DASH_SOURCES src/can_link.c)
    list(APPEND VENDOR_SOURCES "${CAN2040_PATH}/src/can2040.c")
    set(DASH_HAVE_CAN2040 1)
else()
    message(WARNING
        "can2040 not found at ${CAN2040_PATH} - building WITHOUT CAN. "
        "Clone https://github.com/KevinOConnor/can2040 there, or pass "
        "-DCAN2040_PATH=<path>.")
    set(DASH_HAVE_CAN2040 0)
endif()

# Synthetic RPM and boost for the bench - see Simulate() in src/main.c. Off by
# default and never for the car: the values go through the real signal store,
# so they would overwrite genuine telemetry arriving on the bus.
option(DASH_SIMULATE "Feed synthetic RPM and boost into the gauges" OFF)
if(DASH_SIMULATE)
    message(WARNING "DASH_SIMULATE is ON - this build shows SYNTHETIC telemetry, not ECU data.")
endif()

add_executable(dash_node ${DASH_SOURCES} ${VENDOR_SOURCES})

target_compile_definitions(dash_node PRIVATE
    DASH_HAVE_CAN2040=${DASH_HAVE_CAN2040}
    DASH_HAVE_PANEL=${DASH_HAVE_PANEL}
    DASH_SIMULATE=$<BOOL:${DASH_SIMULATE}>
)

target_include_directories(dash_node PRIVATE src)
if(DASH_HAVE_CAN2040)
    target_include_directories(dash_node PRIVATE "${CAN2040_PATH}/src")
endif()
if(DASH_HAVE_PANEL)
    target_include_directories(dash_node PRIVATE vendor)
    target_link_libraries(dash_node hardware_i2c hardware_pwm hardware_spi
                          hardware_watchdog hardware_clocks)

    # Core 1 gets its own stack, declared in main.c, rather than the SDK's.
    # See the comment there: the reserved one lives in SCRATCH_X, which is 4 KB
    # in total on RP2350 and too small. Zero here so the unused reservation is
    # not made at all.
    target_compile_definitions(dash_node PRIVATE PICO_CORE1_STACK_SIZE=0)
endif()

# -Wconversion is deliberate: this code moves between widths constantly -
# 8- and 16-bit CAN fields into int32 signals - and a silent narrowing is
# exactly the class of bug that produces a plausible wrong gauge.
#
# Applied per source rather than to the target, because the SDK's own files are
# compiled into this target too and do not build cleanly under -Wconversion.
# Warnings worth reading are warnings about code we can actually fix.
# Note this is DASH_SOURCES, not VENDOR_SOURCES: can2040 and the Waveshare
# drivers are third-party and would produce a steady stream of narrowing
# warnings nobody here is going to fix, which is exactly how a real warning
# gets lost.
set_source_files_properties(${DASH_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion")

target_link_libraries(dash_node
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_pio
    hardware_dma
    hardware_irq
)

# USB serial for bring-up; the board's UART pins are wanted for CAN.
pico_enable_stdio_usb(dash_node 1)
pico_enable_stdio_uart(dash_node 0)

pico_add_extra_outputs(dash_node)

# ---------------------------------------------------------------------------
# psram_probe - a standalone answer to "is PSRAM actually fitted?"
# ---------------------------------------------------------------------------
#
# Not part of the node firmware. PLAN.md records the PSRAM pad as reserved and
# unpopulated, which is why rendering goes through partial buffers; Waveshare
# ship a full PSRAM driver anyway. This settles which is true, on the board.
#
# vendor/psram_tool.c is Waveshare's detection code - see vendor/README.md for
# provenance and the bugs found while reading their examples.
add_executable(psram_probe
    test/psram_probe.c
    vendor/psram_tool.c
)
target_include_directories(psram_probe PRIVATE vendor)
target_link_libraries(psram_probe
    pico_stdlib
    hardware_clocks
    hardware_gpio
    hardware_spi
    hardware_sync
    hardware_xip_cache
    pico_flash
)
pico_enable_stdio_usb(psram_probe 1)
pico_enable_stdio_uart(psram_probe 0)
pico_add_extra_outputs(psram_probe)
