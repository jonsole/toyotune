/*
 * settings_flash.h
 *
 * The remembered page and views, in the last sector of flash.
 *
 * WRITING FLASH STOPS BOTH CORES USING IT. The chip cannot be read while it
 * is being erased or programmed, and both cores run their code from it, so
 * one core has to hold the other still meanwhile. Here CORE 1 writes and core
 * 0 is held: core 0 must call flash_safe_execute_core_init() first, and it
 * loses roughly a millisecond for an append or tens of milliseconds for the
 * erase that follows every 128th save. A few CAN frames are missed, well
 * inside the half second the store allows before it calls a reading stale -
 * whereas holding core 1 would stop the gauges mid-frame.
 *
 * So SettingsFlash_Save() must be called from core 1, and never from an
 * interrupt.
 */
#ifndef SETTINGS_FLASH_H_
#define SETTINGS_FLASH_H_

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

/* Where the record lives: the last sector of the flash the linker knows
   about, which is far past the image and in the region the board is
   guaranteed to have. */
#define SETTINGS_FLASH_OFFSET	(PICO_FLASH_SIZE_BYTES - SETTINGS_SECTOR_SIZE)

/* Read what was last saved. False if nothing has been, or if the sector is
   not ours, or if this build's image has grown into it - see
   SettingsFlash_Usable(). */
extern bool SettingsFlash_Load(Settings_t *Out);

/* Save, if it differs from what is already there. Core 1 only. False if it
   could not be written, which is reported and then ignored: a node that
   forgets its page is a nuisance, not a fault. */
extern bool SettingsFlash_Save(const Settings_t *In);

/* False when the image reaches into the sector, so nothing may be written.
   Checked rather than assumed, because the faces grow with every page. */
extern bool SettingsFlash_Usable(void);

/* For the status line. */
extern uint32_t SettingsFlash_Saves(void);
extern uint32_t SettingsFlash_Erases(void);
extern uint32_t SettingsFlash_Failures(void);

#endif /* SETTINGS_FLASH_H_ */
