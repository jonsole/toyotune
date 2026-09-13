/*
 * pages.c
 *
 * The page list, the startup assignment, and page selection.
 *
 * Two faces, both a needle gauge: engine speed and boost. The warning page
 * below them is a takeover rather than a third face - see Pages_Effective().
 *
 * Geometry is in percent of the panel rather than pixels, so the same table
 * serves whichever panel the car ends up with - the 1.43" and 1.75" modules
 * are both 466x466, and PLAN.md section 4.8 leaves that choice open until the
 * outline is measured against the 180x50 mm aperture.
 */

#include "node_id.h"
#include "pages.h"
#include "signal_store.h"

/* --- page 0: the rev counter --------------------------------------------- */

/* Marked in thousands, the way a tachometer is. The needle works in rpm; these
   are only what is painted on the face. */
static const char *const RpmTicks[] =
	{ "0", "1", "2", "3", "4", "5", "6", "7", "8", NULL };

static const FaceElement_t RpmElements[] =
{
	{ WIDGET_GAUGE,   SIGNAL_RPM, 0, 8000,  2,  2, 96, 96, RpmTicks },
	{ WIDGET_NUMERIC, SIGNAL_RPM, 0, 8000, 30, 56, 40, 18, NULL }
};

/* --- page 1: boost ------------------------------------------------------- */

/* Manifold pressure absolute, in kPa. The signal is in tenths, so 2500 is
   250.0 kPa - a bit over 1.5 bar of boost. */
static const char *const BoostTicks[] =
	{ "0", "50", "100", "150", "200", "250", NULL };

static const FaceElement_t BoostElements[] =
{
	{ WIDGET_GAUGE,   SIGNAL_MAP, 0, 2500,  2,  2, 96, 96, BoostTicks },
	{ WIDGET_NUMERIC, SIGNAL_MAP, 0, 2500, 25, 56, 50, 18, NULL }
};

/* --- page 2: the warning takeover ----------------------------------------
 *
 * Not reachable by swiping, and kept even though the swipe list is down to
 * two. Pages_Effective() substitutes it while a fault stands, which is why it
 * is here rather than in the list: a driver must not be able to page away
 * from a fault, and making it a normal page would allow exactly that.
 */
static const FaceElement_t WarningElements[] =
{
	{ WIDGET_NUMERIC, SIGNAL_ERROR_FLAGS1,  0,  255, 10, 20, 80, 16, NULL },
	{ WIDGET_NUMERIC, SIGNAL_ERROR_FLAGS2,  0,  255, 10, 38, 80, 16, NULL },
	{ WIDGET_NUMERIC, SIGNAL_LIMITER_FLAGS, 0,  255, 10, 56, 80, 16, NULL },
	{ WIDGET_NUMERIC, SIGNAL_KNOCK_RETARD,  0, 2000, 10, 72, 80, 16, NULL }
};

#define PAGE(name, elems) { name, elems, (uint8_t)(sizeof(elems) / sizeof((elems)[0])) }

const FacePage_t Pages[] =
{
	PAGE("Engine Speed", RpmElements),
	PAGE("Boost",        BoostElements),
	PAGE("WARNING",      WarningElements)
};

const uint8_t PageCount = (uint8_t)(sizeof(Pages) / sizeof(Pages[0]));

/* The warning page is the last entry and is excluded from swiping. */
#define PAGE_WARNING		(2)
#define PAGE_SWIPEABLE_COUNT	(PAGE_WARNING)

/* With two faces and four possible identities, the nodes alternate: a
   three-gauge cluster opens as rev counter, boost, rev counter. Each is still
   swipeable to the other, which is the whole point of the shared list. */
const uint8_t StartupPage[NODE_ID_COUNT] = { 0, 1, 0, 1 };

static uint8_t Current;


/***************************************************************************************/
void Pages_Init(uint8_t NodeId, uint8_t RestoredPage)
{
	/* A restored selection wins, because resetting to the startup page on
	   every ignition cycle would make swiping useless. An out-of-range value
	   - a corrupt or never-written flash record - falls back to the node's
	   startup page rather than refusing to start. */
	if (RestoredPage < PAGE_SWIPEABLE_COUNT)
	{
		Current = RestoredPage;
		return;
	}

	if (NodeId < (uint8_t)(sizeof(StartupPage) / sizeof(StartupPage[0])))
		Current = StartupPage[NodeId];
	else
		Current = 0;	/* unknown identity: start somewhere rather than nowhere */
}


/***************************************************************************************/
uint8_t Pages_Current(void)
{
	return Current;
}


/***************************************************************************************/
void Pages_Next(void)
{
	Current = (uint8_t)((Current + 1u) % PAGE_SWIPEABLE_COUNT);
}


/***************************************************************************************/
void Pages_Previous(void)
{
	Current = (uint8_t)((Current + PAGE_SWIPEABLE_COUNT - 1u) % PAGE_SWIPEABLE_COUNT);
}


/***************************************************************************************/
/* A fault only counts if the signal carrying it is fresh. Raising a warning
   from a stale flag byte would leave the node stuck on the warning face after
   the link dropped, which tells the driver nothing about the engine. */
static bool FlagSetAndFresh(SignalId_t Id, uint32_t NowMs)
{
	SignalReading_t R = SignalStore_Get(Id, NowMs);

	return R.Valid && R.Fresh && (R.Value != 0);
}


bool Pages_WarningActive(uint32_t NowMs)
{
	SignalReading_t Knock;

	if (FlagSetAndFresh(SIGNAL_ERROR_FLAGS1, NowMs) ||
	    FlagSetAndFresh(SIGNAL_ERROR_FLAGS2, NowMs) ||
	    FlagSetAndFresh(SIGNAL_LIMITER_FLAGS, NowMs))
		return true;

	Knock = SignalStore_Get(SIGNAL_KNOCK_RETARD, NowMs);
	if (Knock.Valid && Knock.Fresh && Knock.Value >= PAGES_KNOCK_WARN_DEG100)
		return true;

	return false;
}


/***************************************************************************************/
uint8_t Pages_Effective(uint32_t NowMs)
{
	return Pages_WarningActive(NowMs) ? (uint8_t)PAGE_WARNING : Current;
}
