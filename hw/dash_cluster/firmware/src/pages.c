/*
 * pages.c
 *
 * The page list, the startup assignment, and page selection.
 *
 * Geometry is in percent of the panel rather than pixels, so the same table
 * serves whichever panel the car ends up with - the 1.43" and 1.75" modules
 * are both 466x466, and PLAN.md section 4.8 leaves that choice open until the
 * outline is measured against the 180x50 mm aperture.
 */

#include "node_id.h"
#include "pages.h"
#include "signal_store.h"

/* --- page 0: engine speed ------------------------------------------------ */
static const FaceElement_t RpmElements[] =
{
	{ WIDGET_DIAL,    SIGNAL_RPM,           0, 8000,  5,  5, 90, 90 },
	{ WIDGET_NUMERIC, SIGNAL_RPM,           0, 8000, 30, 40, 40, 20 },
	{ WIDGET_ARC,     SIGNAL_LIMITER_FLAGS, 0,  255,  0,  0, 100, 100 }
};

/* --- page 1: boost ------------------------------------------------------- */
static const FaceElement_t BoostElements[] =
{
	{ WIDGET_DIAL,    SIGNAL_MAP, 0, 2500,  5,  5, 90, 90 },
	{ WIDGET_NUMERIC, SIGNAL_MAP, 0, 2500, 30, 36, 40, 16 },
	{ WIDGET_GRAPH,   SIGNAL_MAP, 0, 2500, 22, 58, 56, 22 }
};

/* --- page 2: health ------------------------------------------------------ */
static const FaceElement_t HealthElements[] =
{
	{ WIDGET_ARC,     SIGNAL_ECT,      -4000, 12000,  5,  5, 90, 90 },
	{ WIDGET_NUMERIC, SIGNAL_ECT,      -4000, 12000, 25, 24, 50, 16 },
	{ WIDGET_NUMERIC, SIGNAL_BATTERY,      0,  1800, 25, 44, 50, 14 },
	{ WIDGET_NUMERIC, SIGNAL_INJ_DUTY,     0, 10000, 25, 60, 50, 14 }
};

/* --- page 3: knock ------------------------------------------------------- */
static const FaceElement_t KnockElements[] =
{
	{ WIDGET_BARGRAPH, SIGNAL_KNOCK_CYL1,      0, 2000, 20, 26, 60, 12 },
	{ WIDGET_BARGRAPH, SIGNAL_KNOCK_CYL2,      0, 2000, 20, 42, 60, 12 },
	{ WIDGET_BARGRAPH, SIGNAL_KNOCK_CYL3,      0, 2000, 20, 58, 60, 12 },
	{ WIDGET_NUMERIC,  SIGNAL_IGN_TIMING_RAW,  0,  255, 30, 74, 40, 12 }
};

/* --- page 4: fuel -------------------------------------------------------- */
static const FaceElement_t FuelElements[] =
{
	{ WIDGET_DIAL,    SIGNAL_INJ_DUTY,        0, 10000,  5,  5, 90, 90 },
	{ WIDGET_NUMERIC, SIGNAL_INJ_PW,          0, 25000, 25, 38, 50, 14 },
	{ WIDGET_NUMERIC, SIGNAL_LAMBDA_RAW,      0,   255, 25, 54, 50, 14 },
	{ WIDGET_NUMERIC, SIGNAL_LAMBDA_TRIM_RAW, 0,   255, 25, 68, 50, 12 }
};

/* --- page 5: the warning takeover ----------------------------------------
 *
 * Not reachable by swiping. Pages_Effective() substitutes it while a fault
 * stands, which is why it is here rather than in the swipe list: a driver
 * must not be able to page away from a fault, and making it a normal page
 * would allow exactly that.
 */
static const FaceElement_t WarningElements[] =
{
	{ WIDGET_NUMERIC, SIGNAL_ERROR_FLAGS1,  0,  255, 10, 20, 80, 16 },
	{ WIDGET_NUMERIC, SIGNAL_ERROR_FLAGS2,  0,  255, 10, 38, 80, 16 },
	{ WIDGET_NUMERIC, SIGNAL_LIMITER_FLAGS, 0,  255, 10, 56, 80, 16 },
	{ WIDGET_NUMERIC, SIGNAL_KNOCK_RETARD,  0, 2000, 10, 72, 80, 16 }
};

#define PAGE(name, elems) { name, elems, (uint8_t)(sizeof(elems) / sizeof((elems)[0])) }

const FacePage_t Pages[] =
{
	PAGE("Engine Speed", RpmElements),
	PAGE("Boost",        BoostElements),
	PAGE("Health",       HealthElements),
	PAGE("Knock",        KnockElements),
	PAGE("Fuel",         FuelElements),
	PAGE("WARNING",      WarningElements)
};

const uint8_t PageCount = (uint8_t)(sizeof(Pages) / sizeof(Pages[0]));

/* The warning page is the last entry and is excluded from swiping. */
#define PAGE_WARNING		(5)
#define PAGE_SWIPEABLE_COUNT	(PAGE_WARNING)

/* Node 0 opens on RPM, node 1 on boost, node 2 on health - a sane cluster at
   power-on. Node 3 exists because the divider gives four identities; it opens
   on knock rather than duplicating a neighbour. */
const uint8_t StartupPage[NODE_ID_COUNT] = { 0, 1, 2, 3 };

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
