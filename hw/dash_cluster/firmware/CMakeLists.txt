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
# LVGL - the widget toolkit
# ---------------------------------------------------------------------------
#
# LVGL 8.x, not 9. Waveshare's CO5300 panel driver and CST9217 touch glue are
# written against 8.1, and porting both is a deliberate piece of work rather
# than a free upgrade - see PLAN.md section 4.1a.
#
# Like can2040, not vendored:
#
#   git clone -b release/v8.3 https://github.com/lvgl/lvgl
#
# ui_model.c does not depend on LVGL and is always built, which is the point of
# splitting it out: the decisions a gauge makes are testable on a host whether
# or not a toolkit is present. Only the thin binding in ui_lvgl.c needs it.
set(LVGL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/lvgl"
    CACHE PATH "Path to an LVGL checkout")

if(EXISTS "${LVGL_PATH}/lvgl.h")
    message(STATUS "LVGL: ${LVGL_PATH}")
    add_subdirectory("${LVGL_PATH}" lvgl_build)
    list(APPEND DASH_SOURCES src/ui_lvgl.c)
    set(DASH_HAVE_LVGL 1)
else()
    message(WARNING
        "LVGL not found at ${LVGL_PATH} - building WITHOUT the display layer. "
        "Clone https://github.com/lvgl/lvgl (branch release/v8.3) there, or "
        "pass -DLVGL_PATH=<path>.")
    set(DASH_HAVE_LVGL 0)
endif()

if(EXISTS "${CAN2040_PATH}/src/can2040.c")
    message(STATUS "can2040: ${CAN2040_PATH}")
    list(APPEND DASH_SOURCES src/can_link.c "${CAN2040_PATH}/src/can2040.c")
    set(DASH_HAVE_CAN2040 1)
else()
    message(WARNING
        "can2040 not found at ${CAN2040_PATH} - building WITHOUT CAN. "
        "Clone https://github.com/KevinOConnor/can2040 there, or pass "
        "-DCAN2040_PATH=<path>.")
    set(DASH_HAVE_CAN2040 0)
endif()

add_executable(dash_node ${DASH_SOURCES})

target_compile_definitions(dash_node PRIVATE
    DASH_HAVE_CAN2040=${DASH_HAVE_CAN2040}
    DASH_HAVE_LVGL=${DASH_HAVE_LVGL}
)

target_include_directories(dash_node PRIVATE src)
if(DASH_HAVE_CAN2040)
    target_include_directories(dash_node PRIVATE "${CAN2040_PATH}/src")
endif()
if(DASH_HAVE_LVGL)
    target_link_libraries(dash_node lvgl)
endif()

# -Wconversion is deliberate: this code moves between widths constantly -
# 8- and 16-bit CAN fields into int32 signals - and a silent narrowing is
# exactly the class of bug that produces a plausible wrong gauge.
#
# Applied per source rather than to the target, because the SDK's own files are
# compiled into this target too and do not build cleanly under -Wconversion.
# Warnings worth reading are warnings about code we can actually fix.
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
