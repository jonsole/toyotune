/*
 * warn.c - which warning is worth a noise. See warn.h.
 */

#include "warn.h"

#include <string.h>

#include "pages.h"
#include "signal_store.h"


/***************************************************************************************/
void Warn_Init(Warn_t *W)
{
	memset(W, 0, sizeof(*W));
}


/***************************************************************************************/
ToneId_t Warn_Tone(WarnId_t Id)
{
	switch (Id)
	{
	case WARN_FAULT:	return TONE_FAULT;
	case WARN_REV:		return TONE_REV;
	case WARN_BOOST:	return TONE_BOOST;
	case WARN_MIXTURE:	return TONE_MIXTURE;
	default:		return TONE_NONE;
	}
}


/***************************************************************************************/
const char *Warn_Name(WarnId_t Id)
{
	switch (Id)
	{
	case WARN_FAULT:	return "fault";
	case WARN_REV:		return "rev limit";
	case WARN_BOOST:	return "overboost";
	case WARN_MIXTURE:	return "lean";
	default:		return "none";
	}
}


/***************************************************************************************/
/* The signal a warning is about, so the page that displays it can be found
   without hard-coding page numbers - a page list is reordered often, and a
   warning that went silent because a page moved would be very hard to
   notice. WARN_FAULT has no one signal: it is the takeover page. */
static SignalId_t Warn_Signal(WarnId_t Id, bool *Any)
{
	*Any = false;
	switch (Id)
	{
	case WARN_REV:		return SIGNAL_RPM;
	case WARN_BOOST:	return SIGNAL_MAP;
	case WARN_MIXTURE:	return SIGNAL_AFR;
	default:
		*Any = true;
		return SIGNAL_RPM;
	}
}


/***************************************************************************************/
static bool Warn_PageShows(uint8_t Page, SignalId_t Signal)
{
	const FacePage_t *P;
	uint32_t i;

	if (Page >= PageCount)
		return false;

	P = &Pages[Page];
	for (i = 0; i < P->ElementCount; i++)
	{
		if (P->Elements[i].Signal == Signal)
			return true;
	}
	return false;
}


/***************************************************************************************/
/* A reading that is both present and arriving. Returns false - and leaves
   Value alone - for anything stale, which is what stops a dropped link
   warning about the last thing it saw. */
static bool Warn_Read(SignalId_t Id, uint32_t NowMs, int32_t *Value)
{
	SignalReading_t R = SignalStore_Get(Id, NowMs);

	if (!R.Valid || !R.Fresh)
		return false;
	*Value = R.Value;
	return true;
}


/***************************************************************************************/
/* One threshold with hysteresis: it comes on above On and goes off below Off,
   and a missing reading turns it off outright. */
static bool Warn_Latch(bool Was, bool Have, int32_t Value, int32_t On, int32_t Off)
{
	if (!Have)
		return false;
	return Was ? (Value >= Off) : (Value >= On);
}


/***************************************************************************************/
static void Warn_Set(Warn_t *W, WarnId_t Id, bool On, uint32_t NowMs)
{
	if (On && !W->On[Id])
	{
		W->Fired[Id]++;
		W->HoldMs[Id] = NowMs + WARN_HOLD_MS;
	}
	W->On[Id] = On;
}


/***************************************************************************************/
WarnId_t Warn_Update(Warn_t *W, uint32_t NowMs, uint8_t Page, uint8_t NodeId)
{
	int32_t Rpm = 0, Map = 0, Afr = 0;
	bool HaveRpm, HaveMap, HaveAfr;
	uint32_t Id;

	HaveRpm = Warn_Read(SIGNAL_RPM, NowMs, &Rpm);
	HaveMap = Warn_Read(SIGNAL_MAP, NowMs, &Map);
	HaveAfr = Warn_Read(SIGNAL_AFR, NowMs, &Afr);

	/* The fault tone and the takeover page are the same decision, asked once,
	   so the screen and the speaker can never disagree about whether there is
	   a fault. */
	Warn_Set(W, WARN_FAULT, Pages_WarningActive(NowMs), NowMs);

	Warn_Set(W, WARN_REV, Warn_Latch(W->On[WARN_REV], HaveRpm, Rpm,
	                                 WARN_REV_ON_RPM, WARN_REV_OFF_RPM), NowMs);

	Warn_Set(W, WARN_BOOST, Warn_Latch(W->On[WARN_BOOST], HaveMap, Map,
	                                   WARN_BOOST_ON_KPA10, WARN_BOOST_OFF_KPA10),
	         NowMs);

	/* Lean, and only while the engine is being asked for something. Both the
	   mixture and the load have their own hysteresis, so neither a needle on
	   a threshold nor a throttle held just off it can chatter. */
	W->Load = Warn_Latch(W->Load, HaveMap, Map, WARN_LOAD_ON_KPA10,
	                     WARN_LOAD_OFF_KPA10);
	Warn_Set(W, WARN_MIXTURE,
	         W->Load && Warn_Latch(W->On[WARN_MIXTURE], HaveAfr, Afr,
	                               WARN_AFR_LEAN_ON_X100, WARN_AFR_LEAN_OFF_X100),
	         NowMs);

	/* Highest priority first - the lowest enumerator wins, which is why they
	   are in that order. Held for WARN_HOLD_MS past the condition clearing. */
	for (Id = (uint32_t)WARN_NONE + 1u; Id < (uint32_t)WARN_COUNT; Id++)
	{
		bool Any;
		SignalId_t Signal = Warn_Signal((WarnId_t)Id, &Any);

		if (!W->On[Id] && (int32_t)(NowMs - W->HoldMs[Id]) >= 0)
			continue;

		/* Nobody sounds a warning about a reading their own face is not
		   showing. The shared page is on every node at once, so there it
		   comes down to identity. */
		if (Any)
		{
			if (NodeId != WARN_SHARED_NODE)
				continue;
		}
		else if (!Warn_PageShows(Page, Signal))
		{
			continue;
		}

		return (WarnId_t)Id;
	}

	return WARN_NONE;
}
