/*
 * settings.h
 *
 * What the node remembers across a power cycle: the page the driver chose and
 * which view each page was left on. Nothing else - this is a gauge cluster's
 * memory of where it was, not a configuration store.
 *
 * WHY A SECTOR OF SLOTS RATHER THAN ONE RECORD
 *
 * Flash erases a whole 4 KB sector at a time, so rewriting one record would
 * mean an erase per change. Instead a record is 32 bytes and is APPENDED to
 * the next free slot in the sector, 128 of them, and only a full sector is
 * erased. Ordinary use - a few page changes a drive - then erases the sector
 * perhaps once a month, and a power cut during an append cannot damage the
 * record before it.
 *
 * The last slot with a good checksum wins. A half-written slot, from a power
 * cut mid-write, fails its checksum and the one before it is used - which is
 * the whole reason for the checksum.
 *
 * Everything here is arithmetic over a buffer, so it is testable on a host.
 * The flash itself is settings_flash.c.
 */
#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#include "pages.h"

#define SETTINGS_SLOT_SIZE	(32u)
#define SETTINGS_SECTOR_SIZE	(4096u)
#define SETTINGS_SLOTS		(SETTINGS_SECTOR_SIZE / SETTINGS_SLOT_SIZE)

/* Bumped if the record's meaning changes. An older or newer version is
   ignored rather than guessed at: starting on the wrong page is a triviality,
   and reading a record as something it is not is not. */
#define SETTINGS_VERSION	(1u)

/* How long the page must sit unchanged before it is written. Long enough that
   swiping through the list is one write, short enough that a driver who picks
   a page and switches off has it remembered. */
#define SETTINGS_SAVE_DELAY_MS	(4000u)

typedef struct
{
	uint8_t Page;			/* the selected page, not the warning takeover */
	uint8_t Views[PAGES_MAX];	/* each page's view */
} Settings_t;

/* Build a slot's 32 bytes. Out must be SETTINGS_SLOT_SIZE. */
extern void Settings_Encode(const Settings_t *In, uint8_t *Out);

/* Read a slot back. False if it is erased, a different version, or its
   checksum does not match. */
extern bool Settings_Decode(const uint8_t *Slot, Settings_t *Out);

/* The last good record in a sector, or false if there is none. */
extern bool Settings_Latest(const uint8_t *Sector, Settings_t *Out);

/* Where the next record goes: the first erased slot, or SETTINGS_SLOTS when
   the sector is full and has to be erased first. */
extern uint32_t Settings_NextSlot(const uint8_t *Sector);

/* True if these differ in anything worth a write. */
extern bool Settings_Differ(const Settings_t *A, const Settings_t *B);

#endif /* SETTINGS_H_ */
