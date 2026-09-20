/*
 * test_settings.c - host tests for what the node remembers across a power cut.
 *
 * The cases that matter are the ones a bench cannot produce on demand: a
 * power cut in the middle of a write, a sector that has never been written,
 * and a sector that is full. Each has to end with the node starting on a
 * sensible page rather than refusing to start or trusting a half-written
 * record.
 */

#include <stdio.h>
#include <string.h>

#include "settings.h"

extern int SettingsTests_Run(int *Checks, int *Failures);

static int Checks;
static int Failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		Checks++;                                                             \
		if (!(cond)) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
			printf(__VA_ARGS__);                                              \
			printf("\n");                                                     \
		}                                                                     \
	} while (0)

static uint8_t Sector[SETTINGS_SECTOR_SIZE];

static void Erase(void)
{
	memset(Sector, 0xFF, sizeof(Sector));
}

static void Append(uint8_t Page, uint8_t View0)
{
	Settings_t S;
	uint32_t Slot = Settings_NextSlot(Sector);

	memset(&S, 0, sizeof(S));
	S.Page = Page;
	S.Views[0] = View0;
	Settings_Encode(&S, &Sector[Slot * SETTINGS_SLOT_SIZE]);
}


/***************************************************************************************/
int SettingsTests_Run(int *OutChecks, int *OutFailures)
{
	Settings_t S, Got;
	uint32_t i;

	Checks = 0;
	Failures = 0;
	printf("settings - the page and views remembered across a power cut\n");

	/* An erased sector - a new board - has nothing, and says so rather than
	   returning something that looks like a page. */
	Erase();
	CHECK(!Settings_Latest(Sector, &Got), "a new sector remembers nothing");
	CHECK(Settings_NextSlot(Sector) == 0u, "and its first slot is free");

	/* A record survives the round trip. */
	memset(&S, 0, sizeof(S));
	S.Page = 3u;
	S.Views[3] = 1u;
	S.Views[0] = 1u;
	Settings_Encode(&S, Sector);
	CHECK(Settings_Decode(Sector, &Got), "a written record reads back");
	CHECK(!Settings_Differ(&S, &Got), "unchanged: page %u views %u%u",
	      Got.Page, Got.Views[0], Got.Views[3]);
	CHECK(Settings_NextSlot(Sector) == 1u, "and the next slot is the one after");

	/* The newest wins, wherever it is in the sector. */
	Erase();
	Append(1u, 0u);
	Append(4u, 1u);
	Append(2u, 0u);
	CHECK(Settings_Latest(Sector, &Got) && Got.Page == 2u,
	      "the last record written is the one used: page %u", Got.Page);

	/* A power cut mid-write: the newest slot is rubbish. The one before it
	   must be used - this is what the checksum is for. */
	Sector[(2u * SETTINGS_SLOT_SIZE) + 7u] ^= 0xFFu;
	CHECK(Settings_Latest(Sector, &Got) && Got.Page == 4u && Got.Views[0] == 1u,
	      "a corrupt newest record falls back to the one before: page %u", Got.Page);

	/* Half a slot written - the first bytes programmed, the rest still
	   erased - is not a record either. */
	Erase();
	Append(5u, 0u);
	memset(&Sector[SETTINGS_SLOT_SIZE], 0x00, 6u);
	CHECK(Settings_Latest(Sector, &Got) && Got.Page == 5u,
	      "a half-written slot is ignored: page %u", Got.Page);

	/* A sector full of records: the caller is told to erase. */
	Erase();
	for (i = 0; i < SETTINGS_SLOTS; i++)
		Append((uint8_t)(i % 6u), 0u);
	CHECK(Settings_NextSlot(Sector) == SETTINGS_SLOTS, "a full sector has no free slot");
	CHECK(Settings_Latest(Sector, &Got)
	      && Got.Page == (uint8_t)((SETTINGS_SLOTS - 1u) % 6u),
	      "and its newest record is still readable: page %u", Got.Page);

	/* 128 saves between erases - the reason for slots at all. */
	{
		uint32_t Slots = SETTINGS_SLOTS;

		CHECK(Slots == 128u, "128 slots to a sector, got %u", (unsigned)Slots);
	}

	/* Another firmware's sector, or a stray erase pattern, is not ours. */
	Erase();
	memset(Sector, 0x5A, SETTINGS_SLOT_SIZE);
	CHECK(!Settings_Latest(Sector, &Got), "someone else's data is not a record");

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
