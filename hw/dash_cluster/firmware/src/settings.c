/*
 * settings.c - the remembered page and views, as records. See settings.h.
 */

#include "settings.h"

#include <string.h>

/* "TTS" and a version, so a sector that has held something else is not read
   as ours. */
#define SETTINGS_MAGIC		(0x54545301u)

#define OFF_MAGIC		(0u)
#define OFF_VERSION		(4u)
#define OFF_PAGE		(5u)
#define OFF_VIEWS		(6u)
#define OFF_HASH		(SETTINGS_SLOT_SIZE - 4u)


/***************************************************************************************/
/* FNV-1a over the slot's first 28 bytes. Not a CRC: this guards against a
   half-written slot and a sector that was never ours, both of which change
   many bytes, and it is a dozen instructions. */
static uint32_t Settings_Hash(const uint8_t *Slot)
{
	uint32_t H = 2166136261u;
	uint32_t i;

	for (i = 0; i < OFF_HASH; i++)
	{
		H ^= Slot[i];
		H *= 16777619u;
	}
	return H;
}


/***************************************************************************************/
static uint32_t Settings_Get32(const uint8_t *P)
{
	return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16)
	       | ((uint32_t)P[3] << 24);
}

static void Settings_Put32(uint8_t *P, uint32_t V)
{
	P[0] = (uint8_t)(V & 0xFFu);
	P[1] = (uint8_t)((V >> 8) & 0xFFu);
	P[2] = (uint8_t)((V >> 16) & 0xFFu);
	P[3] = (uint8_t)((V >> 24) & 0xFFu);
}


/***************************************************************************************/
void Settings_Encode(const Settings_t *In, uint8_t *Out)
{
	uint32_t i;

	memset(Out, 0, SETTINGS_SLOT_SIZE);
	Settings_Put32(&Out[OFF_MAGIC], SETTINGS_MAGIC);
	Out[OFF_VERSION] = (uint8_t)SETTINGS_VERSION;
	Out[OFF_PAGE] = In->Page;
	for (i = 0; i < PAGES_MAX && (OFF_VIEWS + i) < OFF_HASH; i++)
		Out[OFF_VIEWS + i] = In->Views[i];

	Settings_Put32(&Out[OFF_HASH], Settings_Hash(Out));
}


/***************************************************************************************/
bool Settings_Decode(const uint8_t *Slot, Settings_t *Out)
{
	uint32_t i;

	if (Settings_Get32(&Slot[OFF_MAGIC]) != SETTINGS_MAGIC)
		return false;
	if (Slot[OFF_VERSION] != (uint8_t)SETTINGS_VERSION)
		return false;
	if (Settings_Get32(&Slot[OFF_HASH]) != Settings_Hash(Slot))
		return false;

	memset(Out, 0, sizeof(*Out));
	Out->Page = Slot[OFF_PAGE];
	for (i = 0; i < PAGES_MAX && (OFF_VIEWS + i) < OFF_HASH; i++)
		Out->Views[i] = Slot[OFF_VIEWS + i];
	return true;
}


/***************************************************************************************/
bool Settings_Latest(const uint8_t *Sector, Settings_t *Out)
{
	uint32_t i = SETTINGS_SLOTS;

	/* Backwards: records are appended, so the last good one is the newest.
	   Anything after it is erased, or the wreck of a power cut mid-write. */
	while (i > 0u)
	{
		i--;
		if (Settings_Decode(&Sector[i * SETTINGS_SLOT_SIZE], Out))
			return true;
	}
	return false;
}


/***************************************************************************************/
uint32_t Settings_NextSlot(const uint8_t *Sector)
{
	uint32_t i, b;

	for (i = 0; i < SETTINGS_SLOTS; i++)
	{
		const uint8_t *Slot = &Sector[i * SETTINGS_SLOT_SIZE];
		bool Erased = true;

		for (b = 0; b < SETTINGS_SLOT_SIZE; b++)
		{
			if (Slot[b] != 0xFFu)
			{
				Erased = false;
				break;
			}
		}
		if (Erased)
			return i;
	}
	return SETTINGS_SLOTS;
}


/***************************************************************************************/
bool Settings_Differ(const Settings_t *A, const Settings_t *B)
{
	return memcmp(A, B, sizeof(*A)) != 0;
}
