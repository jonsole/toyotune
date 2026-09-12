/**
 * @file psram_tools.h
 *
 * @brief Header file for a function that is used to detect and initialize PSRAM on
 *  4D Systems RP2350 Displays.
 */

/*

*/

#ifndef _PSRAM_TOOLS_H_
#define _PSRAM_TOOLS_H_
#include <stdint.h>
#include <stdlib.h>
#define RP2350_XIP_CSI_PIN 47

/// @brief The setup_psram function - note that this is not in flash
///
/// @param psram_cs_pin The pin that the PSRAM is connected to
/// @return size_t The size of the PSRAM
///
size_t rp_setup_psram(uint32_t psram_cs_pin);

/// @brief The rp_psram_update_timing function - note that this is not in flash
///
/// @note - updates the PSRAM QSPI timing - call if the system clock is changed after PSRAM is initialized
///
void rp_psram_update_timing(void);
#endif