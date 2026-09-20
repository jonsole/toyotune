/*
 * sdlog.c - the ECU log on the microSD card. See sdlog.h.
 */

#include "sdlog.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "f_util.h"
#include "ff.h"
#include "hw_config.h"

#include "logfmt.h"
#include "signal_store.h"

/* How long the link must be quiet before a file is closed. Longer than the
   store's own "link dead", so a gap in the traffic does not chop a drive into
   a heap of files. */
#define SDLOG_CLOSE_AFTER_MS	(5000u)

/* Retry a card that would not mount this often, rather than every pass. */
#define SDLOG_RETRY_MS		(5000u)

static bool	Mounted;
static bool	Open;
static FATFS	Fs;
static FIL	File;
static char	Name[16];

static char	Buffer[SDLOG_BUFFER];
static uint32_t	Used;

static uint32_t	StartMs;		/* when this file was opened */
static uint32_t	NextRowMs;
static uint32_t	NextSyncMs;
static uint32_t	LastDataMs;
static uint32_t	RetryMs;

static uint32_t	Rows;
static uint32_t	Bytes;
static uint32_t	Errors;


/***************************************************************************************/
void SdLog_Init(void)
{
	Mounted = false;
	Open = false;
	Used = 0u;
	Name[0] = '\0';
}


/***************************************************************************************/
static bool SdLog_CardPresent(void)
{
	sd_card_t *Card = sd_get_by_num(0);

	return Card != NULL && sd_card_detect(Card);
}


/***************************************************************************************/
/* The next free LOGnnnn.CSV. Numbered rather than dated because the node's
   clock comes from the bus and may not have arrived yet - and a log called
   LOG0007 is honest about that, where a wrong date would not be. */
static bool SdLog_NextName(void)
{
	FILINFO Info;
	uint32_t n;

	for (n = 1; n <= 9999u; n++)
	{
		(void)snprintf(Name, sizeof(Name), "LOG%04lu.CSV", (unsigned long)n);
		if (f_stat(Name, &Info) != FR_OK)
			return true;
	}

	Name[0] = '\0';
	return false;
}


/***************************************************************************************/
static void SdLog_Close(const char *Why)
{
	if (!Open)
		return;

	if (Used > 0u)
	{
		UINT Wrote = 0;

		(void)f_write(&File, Buffer, Used, &Wrote);
		Bytes += Wrote;
		Used = 0u;
	}
	(void)f_close(&File);
	Open = false;
	printf("sdlog: closed %s after %lu rows (%s)\n", Name, (unsigned long)Rows, Why);
}


/***************************************************************************************/
static bool SdLog_Open(uint32_t NowMs)
{
	char Line[LOGFMT_LINE_MAX];
	UINT Wrote = 0;
	uint32_t Len;

	if (!SdLog_NextName())
	{
		printf("sdlog: the card already holds LOG0001 to LOG9999\n");
		Errors++;
		return false;
	}

	if (f_open(&File, Name, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		Errors++;
		return false;
	}

	Len = LogFmt_Header(Line, sizeof(Line));
	if (Len >= sizeof(Line))
		Len = (uint32_t)strlen(Line);		/* truncated - write what fits */
	if (f_write(&File, Line, Len, &Wrote) != FR_OK)
	{
		(void)f_close(&File);
		Errors++;
		return false;
	}

	Bytes += Wrote;
	Open = true;
	Rows = 0u;
	Used = 0u;
	StartMs = NowMs;
	NextRowMs = NowMs;
	NextSyncMs = NowMs + SDLOG_SYNC_MS;
	printf("sdlog: logging to %s\n", Name);
	return true;
}


/***************************************************************************************/
/* Everything gathered so far, onto the card. */
static bool SdLog_Flush(bool Sync)
{
	UINT Wrote = 0;

	if (Used > 0u)
	{
		if (f_write(&File, Buffer, Used, &Wrote) != FR_OK || Wrote != Used)
		{
			Errors++;
			return false;
		}
		Bytes += Wrote;
		Used = 0u;
	}

	/* f_sync is what makes the file readable if the ignition goes off: it
	   puts the directory entry and the allocation table on the card. */
	if (Sync && f_sync(&File) != FR_OK)
	{
		Errors++;
		return false;
	}
	return true;
}


/***************************************************************************************/
void SdLog_Poll(uint32_t NowMs)
{
	bool Alive = SignalStore_LinkAlive(NowMs);

	if (Alive)
		LastDataMs = NowMs;

	/* ---- the card ------------------------------------------------- */

	if (!SdLog_CardPresent())
	{
		if (Open)
			SdLog_Close("card removed");
		if (Mounted)
		{
			(void)f_unmount("");
			Mounted = false;
		}
		return;
	}

	if (!Mounted)
	{
		if ((int32_t)(NowMs - RetryMs) < 0)
			return;
		RetryMs = NowMs + SDLOG_RETRY_MS;

		if (f_mount(&Fs, "", 1) != FR_OK)
		{
			Errors++;
			return;
		}
		Mounted = true;
		printf("sdlog: card mounted\n");
	}

	/* ---- the file ------------------------------------------------- */

	if (!Open)
	{
		/* A drive is a file: opened when the ECU starts talking. */
		if (Alive && !SdLog_Open(NowMs))
			RetryMs = NowMs + SDLOG_RETRY_MS;
		return;
	}

	if ((int32_t)(NowMs - LastDataMs) > (int32_t)SDLOG_CLOSE_AFTER_MS)
	{
		SdLog_Close("link down");
		return;
	}

	/* ---- a row ---------------------------------------------------- */

	if ((int32_t)(NowMs - NextRowMs) >= 0)
	{
		char Line[LOGFMT_LINE_MAX];
		uint32_t Len;

		NextRowMs += SDLOG_PERIOD_MS;
		if ((int32_t)(NowMs - NextRowMs) > (int32_t)SDLOG_PERIOD_MS)
			NextRowMs = NowMs + SDLOG_PERIOD_MS;	/* fell behind; do not catch up */

		Len = LogFmt_Row(Line, sizeof(Line), NowMs - StartMs, NowMs);
		if (Len >= sizeof(Line))
			Len = (uint32_t)strlen(Line);

		if (Used + Len > sizeof(Buffer) && !SdLog_Flush(false))
		{
			SdLog_Close("write failed");
			return;
		}
		if (Used + Len <= sizeof(Buffer))
		{
			memcpy(&Buffer[Used], Line, Len);
			Used += Len;
			Rows++;
		}
	}

	if ((int32_t)(NowMs - NextSyncMs) >= 0)
	{
		NextSyncMs = NowMs + SDLOG_SYNC_MS;
		if (!SdLog_Flush(true))
			SdLog_Close("sync failed");
	}
}


/***************************************************************************************/
bool SdLog_Active(void)
{
	return Open;
}


const char *SdLog_FileName(void)
{
	return Name;
}


uint32_t SdLog_Rows(void)
{
	return Rows;
}


uint32_t SdLog_Bytes(void)
{
	return Bytes;
}


uint32_t SdLog_Errors(void)
{
	return Errors;
}
