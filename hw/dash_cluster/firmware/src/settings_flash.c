/*
 * settings_flash.c - the remembered page and views, in flash. See the header.
 */

#include "settings_flash.h"

#include <stdio.h>
#include <string.h>

#include "pico/flash.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"

/* The end of everything the linker placed in flash, from the SDK's script. */
extern char __flash_binary_end;

static uint32_t Saves;
static uint32_t Erases;
static uint32_t Failures;

/* What flash_safe_execute() is asked to do, since it takes one pointer. */
typedef struct
{
	uint32_t Offset;		/* within flash */
	const uint8_t *Data;		/* NULL to erase the sector */
} SettingsWrite_t;


/***************************************************************************************/
static const uint8_t *SettingsFlash_Sector(void)
{
	return (const uint8_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
}


/***************************************************************************************/
bool SettingsFlash_Usable(void)
{
	uint32_t ImageEnd = (uint32_t)&__flash_binary_end - XIP_BASE;

	return ImageEnd <= SETTINGS_FLASH_OFFSET;
}


/***************************************************************************************/
bool SettingsFlash_Load(Settings_t *Out)
{
	if (!SettingsFlash_Usable())
		return false;
	return Settings_Latest(SettingsFlash_Sector(), Out);
}


/***************************************************************************************/
/* Runs with the other core held and interrupts off - so it does the flash
   operation and nothing else. */
static void SettingsFlash_Do(void *Param)
{
	const SettingsWrite_t *W = (const SettingsWrite_t *)Param;

	if (W->Data == NULL)
		flash_range_erase(W->Offset, SETTINGS_SECTOR_SIZE);
	else
		flash_range_program(W->Offset, W->Data, FLASH_PAGE_SIZE);
}


/***************************************************************************************/
bool SettingsFlash_Save(const Settings_t *In)
{
	/* A flash page is the smallest thing that can be programmed - 256 bytes,
	   eight slots. The seven after the one being written stay erased, so the
	   next save can still use them. */
	static uint8_t Page[FLASH_PAGE_SIZE];
	SettingsWrite_t Write;
	Settings_t Current;
	uint32_t Slot;

	if (!SettingsFlash_Usable())
		return false;

	if (Settings_Latest(SettingsFlash_Sector(), &Current) && !Settings_Differ(&Current, In))
		return true;			/* already what is there */

	Slot = Settings_NextSlot(SettingsFlash_Sector());
	if (Slot >= SETTINGS_SLOTS)
	{
		/* Full: erase and start again at the first slot. The record being
		   saved is about to be written, so nothing is lost. */
		Write.Offset = SETTINGS_FLASH_OFFSET;
		Write.Data = NULL;
		if (flash_safe_execute(SettingsFlash_Do, &Write, 2000u) != PICO_OK)
		{
			Failures++;
			return false;
		}
		Erases++;
		Slot = 0u;
	}

	/* Program the whole page the slot is in, with the slot's bytes in place
	   and the rest left erased. */
	memset(Page, 0xFF, sizeof(Page));
	Settings_Encode(In, &Page[(Slot * SETTINGS_SLOT_SIZE) % FLASH_PAGE_SIZE]);

	Write.Offset = SETTINGS_FLASH_OFFSET
	               + (((Slot * SETTINGS_SLOT_SIZE) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE);
	Write.Data = Page;
	if (flash_safe_execute(SettingsFlash_Do, &Write, 2000u) != PICO_OK)
	{
		Failures++;
		return false;
	}

	Saves++;
	return true;
}


/***************************************************************************************/
uint32_t SettingsFlash_Saves(void)
{
	return Saves;
}


uint32_t SettingsFlash_Erases(void)
{
	return Erases;
}


uint32_t SettingsFlash_Failures(void)
{
	return Failures;
}
