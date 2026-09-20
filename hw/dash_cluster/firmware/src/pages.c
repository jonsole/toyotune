/*
 * pages.c
 *
 * The page list, the startup assignment, and page selection.
 *
 * Six pages: the rev counter, boost over mixture, mixture over exhaust
 * temperature, intake over manifold air temperature, the g-force circle and
 * the clock. Each has a second view a vertical swipe away - a strip chart of
 * the same readings, or for the clock, the time in figures. The warning page
 * below them is a takeover rather than a seventh page - see
 * Pages_Effective().
 *
 * Geometry is in percent of the panel rather than pixels, so the same table
 * serves whichever panel the car ends up with - the 1.43" and 1.75" modules
 * are both 466x466, and PLAN.md section 4.8 leaves that choice open until the
 * outline is measured against the 180x50 mm aperture.
 */

#include <stdio.h>

#include "node_id.h"
#include "pages.h"
#include "signal_store.h"

/* --- page 0: the rev counter ---------------------------------------------
 *
 * One element per face. A gauge carries its own value, centred in the empty
 * middle of the dial, and its own label at the foot of it - UiLvgl_BuildElement()
 * gives every widget both. A separate WIDGET_NUMERIC for the same signal used
 * to sit below it and put the value and the label on the glass twice.
 */

/* Marked in thousands, the way a tachometer is. The needle works in rpm; these
   are only what is painted on the face. */
static const char *const RpmTicks[] =
	{ "0", "1", "2", "3", "4", "5", "6", "7", "8", NULL };

static const FaceElement_t RpmElements[] =
{
	{ WIDGET_GAUGE,   SIGNAL_RPM, 0, 8000,  2,  2, 96, 96, RpmTicks, "x1000r/min", 7000, GAUGE_SWEEP_FULL, NULL }
};

/* The graph views all follow one rule: the same signals, ranges, scale labels
   and formatting as the gauges they flip with, so a reading means the same on
   either side of the swipe. Red is the first trace, white the second. */
static const FaceElement_t RpmGraph[] =
{
	{ WIDGET_GRAPH,   SIGNAL_RPM, 0, 8000,  2,  2, 96, 96, RpmTicks, "x1000r/min", 0, GAUGE_SWEEP_FULL, NULL }
};

/* --- page 1: boost over AFR ---------------------------------------------- *
 *
 * Two half gauges on one face: boost across the top, the wideband's mixture
 * across the bottom - the two numbers that matter together under load.
 */

/* Boost as a gauge reads it: bar above atmospheric, vacuum below zero. The
   sensor measures ABSOLUTE pressure in tenths of a kPa, so the scale is
   offset by a standard atmosphere. That is an assumption - the true ambient
   pressure moves with the weather and the altitude by a few kPa - and it is
   the same one every boost gauge plumbed to a manifold makes. */
#define BOOST_ATMOSPHERE	(1013)		/* 101.3 kPa, tenths */
#define BOOST_KPA10_PER_BAR	(1000)

static const char *const BoostTicks[] =
	{ "-1", "-0.5", "0", "0.5", "1", "1.5", NULL };

/* The reading, in hundredths of a bar: "1.23", "-0.45". Integer arithmetic,
   rounded to the nearest hundredth. */
static const char *Pages_FormatBoost(int32_t Value, char *Out, uint32_t OutSize)
{
	int32_t Gauge = Value - BOOST_ATMOSPHERE;	/* tenths of a kPa above atmosphere */
	int32_t Centi, Mag;

	/* A bar is 1000 tenths of a kPa, so a hundredth of a bar is ten of them. */
	Centi = (Gauge >= 0) ? ((Gauge + 5) / 10) : -(((-Gauge) + 5) / 10);
	Mag = (Centi < 0) ? -Centi : Centi;
	(void)snprintf(Out, OutSize, "%s%ld.%02ld", (Centi < 0) ? "-" : "",
	               (long)(Mag / 100), (long)(Mag % 100));
	return Out;
}

/* AFR to one decimal: the wideband's hundredths are more than a reading can
   usefully show, and the second digit would never settle. */
static const char *Pages_FormatAfr(int32_t Value, char *Out, uint32_t OutSize)
{
	int32_t Tenths = (Value + 5) / 10;

	(void)snprintf(Out, OutSize, "%ld.%ld", (long)(Tenths / 10), (long)(Tenths % 10));
	return Out;
}

/* Gasoline AFR, rich on the left and lean on the right. */
static const char *const AfrTicks[] =
	{ "10", "12", "14", "16", "18", "20", NULL };

static const FaceElement_t BoostElements[] =
{
	/* -1.0 to +1.5 bar, red from +1.0. */
	{ WIDGET_GAUGE, SIGNAL_MAP,
	  BOOST_ATMOSPHERE - BOOST_KPA10_PER_BAR,
	  BOOST_ATMOSPHERE + (3 * BOOST_KPA10_PER_BAR) / 2,
	  2, 2, 96, 96, BoostTicks, "bar",
	  BOOST_ATMOSPHERE + BOOST_KPA10_PER_BAR,
	  GAUGE_SWEEP_TOP, Pages_FormatBoost },
	{ WIDGET_GAUGE, SIGNAL_AFR, 1000, 2000,
	  2, 2, 96, 96, AfrTicks, "AFR", 0,
	  GAUGE_SWEEP_BOTTOM, Pages_FormatAfr }
};

/* --- page 2: mixture over exhaust temperature -----------------------------
 *
 * Both from the Spartan 3 wideband: the mixture across the top on the same
 * scale as page 1, and the exhaust gas temperature under it. Together they
 * say whether the engine is being fuelled for the load it is under - lean
 * shows on one and the heat it makes on the other.
 *
 * Red from 900 C. Where that should sit depends on where the thermocouple is
 * - pre-turbine reads hotter than a downpipe - so it is a placeholder until
 * the probe is fitted.
 */
static const char *const EgtTicks[] =
	{ "0", "200", "400", "600", "800", "1000", NULL };

static const FaceElement_t AfrEgtElements[] =
{
	{ WIDGET_GAUGE, SIGNAL_AFR, 1000, 2000,
	  2, 2, 96, 96, AfrTicks, "AFR", 0,
	  GAUGE_SWEEP_TOP, Pages_FormatAfr },
	{ WIDGET_GAUGE, SIGNAL_EGT, 0, 1000,
	  2, 2, 96, 96, EgtTicks, "EGT \xC2\xB0" "C", 900,
	  GAUGE_SWEEP_BOTTOM, NULL }
};

static const FaceElement_t AfrEgtGraph[] =
{
	{ WIDGET_GRAPH, SIGNAL_AFR, 1000, 2000,
	  2, 2, 96, 96, AfrTicks, "AFR", 0, GAUGE_SWEEP_FULL, Pages_FormatAfr },
	{ WIDGET_GRAPH, SIGNAL_EGT, 0, 1000,
	  2, 2, 96, 96, EgtTicks, "EGT \xC2\xB0" "C", 0, GAUGE_SWEEP_FULL, NULL }
};

/* --- page 3: intake over manifold air temperature ----------------------------
 *
 * The ECU's two air temperature sensors, THA and THAM, on the same scale so
 * the gap between them reads straight off the face: intake across the top,
 * the charge in the manifold under it. Red from 60 C on the manifold only -
 * a hot charge is where knock comes from, and the intake reading on its own
 * is mostly the weather.
 *
 * Minus 20 to 100, because a British winter morning is below zero and a
 * needle pinned at the stop would read as a fault.
 */
static const char *const AirTempTicks[] =
	{ "-20", "0", "20", "40", "60", "80", "100", NULL };

/* Whole degrees from the hundredths the ECU sends, rounded - a tenth of a
   degree of intake air is nothing a driver can act on, and it would never
   settle. */
static const char *Pages_FormatTemp(int32_t Value, char *Out, uint32_t OutSize)
{
	int32_t Deg = (Value >= 0) ? ((Value + 50) / 100) : -(((-Value) + 50) / 100);

	(void)snprintf(Out, OutSize, "%ld", (long)Deg);
	return Out;
}

static const FaceElement_t AirTempElements[] =
{
	{ WIDGET_GAUGE, SIGNAL_THA, -2000, 10000,
	  2, 2, 96, 96, AirTempTicks, "Intake \xC2\xB0" "C", PAGES_NO_BAND,
	  GAUGE_SWEEP_TOP, Pages_FormatTemp },
	{ WIDGET_GAUGE, SIGNAL_THAM, -2000, 10000,
	  2, 2, 96, 96, AirTempTicks, "Manifold \xC2\xB0" "C", 6000,
	  GAUGE_SWEEP_BOTTOM, Pages_FormatTemp }
};

static const FaceElement_t AirTempGraph[] =
{
	{ WIDGET_GRAPH, SIGNAL_THA, -2000, 10000,
	  2, 2, 96, 96, AirTempTicks, "Intake \xC2\xB0" "C", 0,
	  GAUGE_SWEEP_FULL, Pages_FormatTemp },
	{ WIDGET_GRAPH, SIGNAL_THAM, -2000, 10000,
	  2, 2, 96, 96, AirTempTicks, "Manifold \xC2\xB0" "C", 0,
	  GAUGE_SWEEP_FULL, Pages_FormatTemp }
};

/* --- page 4: g-force --------------------------------------------------------
 *
 * A friction circle from the node's own accelerometer, +-1.5 g. Min and Max
 * are the scale in thousandths of a g; the element has no ticks - its rings
 * are drawn by the face renderer from ui_gauge.h.
 */
static const FaceElement_t GForceElements[] =
{
	{ WIDGET_GFORCE, SIGNAL_G_LAT, -1500, 1500, 2, 2, 96, 96, NULL, NULL, 0,
	  GAUGE_SWEEP_FULL, NULL }
};

/* Its trace: cornering in red, acceleration and braking in white, over the
   same +-1.5 g. Where the circle shows the shape of a corner, this shows its
   timing - the brake coming off as the turn goes in. */
static const char *const GTicks[] =
	{ "-1.5", "-1", "-0.5", "0", "0.5", "1", "1.5", NULL };

/* Hundredths of a g from thousandths, rounded, signed. */
static const char *Pages_FormatG(int32_t Value, char *Out, uint32_t OutSize)
{
	int32_t Centi = (Value >= 0) ? ((Value + 5) / 10) : -(((-Value) + 5) / 10);
	int32_t Mag = (Centi < 0) ? -Centi : Centi;

	(void)snprintf(Out, OutSize, "%s%ld.%02ld", (Centi < 0) ? "-" : "",
	               (long)(Mag / 100), (long)(Mag % 100));
	return Out;
}

static const FaceElement_t GForceGraph[] =
{
	{ WIDGET_GRAPH, SIGNAL_G_LAT, -1500, 1500, 2, 2, 96, 96, GTicks, "lat g", 0,
	  GAUGE_SWEEP_FULL, Pages_FormatG },
	{ WIDGET_GRAPH, SIGNAL_G_LON, -1500, 1500, 2, 2, 96, 96, GTicks, "lon g", 0,
	  GAUGE_SWEEP_FULL, Pages_FormatG }
};

/* --- page 1's graph view: boost and mixture over time ------------------------
 *
 * The same two signals as page 1, as a scrolling trace: boost in red against
 * the left-hand scale, mixture in white against the right. A needle says what
 * is happening now; this says what just happened, which is where a lean spike
 * on a gearchange or boost falling away at the top of a gear shows up.
 *
 * It used to be a page of its own. As a view of page 1 it is one swipe up
 * from the needles showing the same thing, rather than four pages away.
 */
static const FaceElement_t BoostGraph[] =
{
	{ WIDGET_GRAPH, SIGNAL_MAP,
	  BOOST_ATMOSPHERE - BOOST_KPA10_PER_BAR,
	  BOOST_ATMOSPHERE + (3 * BOOST_KPA10_PER_BAR) / 2,
	  2, 2, 96, 96, BoostTicks, "bar", 0, GAUGE_SWEEP_FULL, Pages_FormatBoost },
	{ WIDGET_GRAPH, SIGNAL_AFR, 1000, 2000,
	  2, 2, 96, 96, AfrTicks, "AFR", 0, GAUGE_SWEEP_FULL, Pages_FormatAfr }
};

/* --- page 5: the clock -----------------------------------------------------
 *
 * Hands, from the RTC - which this board does not back up, so the time comes
 * from the bus (clock_link.h) or the console and is lost at every power-off.
 * A clock with no trustworthy time shows no hands and reads "--:--".
 */
static const char *const ClockTicks[] =
	{ "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", NULL };

static const FaceElement_t ClockElements[] =
{
	{ WIDGET_CLOCK, SIGNAL_CLOCK, 0, 86399, 2, 2, 96, 96, ClockTicks, NULL, 0,
	  GAUGE_SWEEP_CLOCK, NULL }
};

/* And in figures: hours and minutes large, seconds small beneath. */
static const FaceElement_t ClockDigital[] =
{
	{ WIDGET_CLOCK_DIGITAL, SIGNAL_CLOCK, 0, 86399, 2, 2, 96, 96, NULL, NULL, 0,
	  GAUGE_SWEEP_FULL, NULL }
};

/* --- page 6: the warning takeover ----------------------------------------
 *
 * Not reachable by swiping. Pages_Effective() substitutes it while a fault
 * stands, which is why it is here rather than in the list: a driver must not
 * be able to page away from a fault, and making it a normal page would allow
 * exactly that.
 */
static const FaceElement_t WarningElements[] =
{
	{ WIDGET_NUMERIC, SIGNAL_ERROR_FLAGS1,  0,  255, 10, 20, 80, 16, NULL, NULL, 0, GAUGE_SWEEP_FULL, NULL },
	{ WIDGET_NUMERIC, SIGNAL_ERROR_FLAGS2,  0,  255, 10, 38, 80, 16, NULL, NULL, 0, GAUGE_SWEEP_FULL, NULL },
	{ WIDGET_NUMERIC, SIGNAL_LIMITER_FLAGS, 0,  255, 10, 56, 80, 16, NULL, NULL, 0, GAUGE_SWEEP_FULL, NULL },
	{ WIDGET_NUMERIC, SIGNAL_KNOCK_RETARD,  0, 2000, 10, 72, 80, 16, NULL, NULL, 0, GAUGE_SWEEP_FULL, NULL }
};

#define COUNT(elems)	((uint8_t)(sizeof(elems) / sizeof((elems)[0])))
#define PAGE(name, elems)	{ name, elems, COUNT(elems), NULL, 0u }
#define PAGE2(name, elems, alt)	{ name, elems, COUNT(elems), alt, COUNT(alt) }

const FacePage_t Pages[] =
{
	PAGE2("Engine Speed", RpmElements,     RpmGraph),
	PAGE2("Boost / AFR",  BoostElements,   BoostGraph),
	PAGE2("AFR / EGT",    AfrEgtElements,  AfrEgtGraph),
	PAGE2("Air temps",    AirTempElements, AirTempGraph),
	PAGE2("G-force",      GForceElements,  GForceGraph),
	PAGE2("Clock",        ClockElements,   ClockDigital),
	PAGE("WARNING",       WarningElements)
};

const uint8_t PageCount = (uint8_t)(sizeof(Pages) / sizeof(Pages[0]));

/* The warning page is the last entry and is excluded from swiping. */
#define PAGE_WARNING		(6)
#define PAGE_SWIPEABLE_COUNT	(PAGE_WARNING)

/* Six pages and a three-gauge cluster: each node opens on a different one -
   rev counter, boost and mixture, g-force - and a fourth identity on mixture
   and exhaust temperature; the rest are a swipe away from any of them. Each is
   still swipeable to the others, which is the whole point of the shared list.
   Every page opens on its first view. */
const uint8_t StartupPage[NODE_ID_COUNT] = { 0, 1, 4, 2 };

static uint8_t Current;
static uint8_t View[PAGES_MAX];		/* each page's view, 0 or 1 */


/***************************************************************************************/
void Pages_Init(uint8_t NodeId, uint8_t RestoredPage)
{
	uint8_t p;

	for (p = 0; p < PAGES_MAX; p++)
		View[p] = 0u;

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
uint8_t Pages_Neighbour(int Direction)
{
	if (Direction > 0)
		return (uint8_t)((Current + 1u) % PAGE_SWIPEABLE_COUNT);
	return (uint8_t)((Current + PAGE_SWIPEABLE_COUNT - 1u) % PAGE_SWIPEABLE_COUNT);
}


/***************************************************************************************/
void Pages_Previous(void)
{
	Current = (uint8_t)((Current + PAGE_SWIPEABLE_COUNT - 1u) % PAGE_SWIPEABLE_COUNT);
}


/***************************************************************************************/
void Pages_Restore(uint8_t Page, const uint8_t *Views)
{
	uint8_t p;

	if (Page < PAGE_SWIPEABLE_COUNT)
		Current = Page;

	for (p = 0; p < PAGES_MAX; p++)
		View[p] = (Views[p] == 1u) ? 1u : 0u;
}


/***************************************************************************************/
void Pages_Snapshot(uint8_t *Page, uint8_t *Views)
{
	uint8_t p;

	*Page = Current;
	for (p = 0; p < PAGES_MAX; p++)
		Views[p] = View[p];
}


/***************************************************************************************/
bool Pages_HasAlt(uint8_t Page)
{
	return Page < PageCount && Page < PAGES_MAX && Pages[Page].AltElements != NULL
	       && Pages[Page].AltElementCount != 0u;
}


/***************************************************************************************/
uint8_t Pages_ViewOf(uint8_t Page)
{
	return Pages_HasAlt(Page) ? View[Page] : 0u;
}


/***************************************************************************************/
void Pages_Flip(void)
{
	if (Pages_HasAlt(Current))
		View[Current] = (uint8_t)(View[Current] ^ 1u);
}


/***************************************************************************************/
const FaceElement_t *Pages_Elements(uint8_t Page, uint8_t ViewIndex, uint8_t *Count)
{
	*Count = 0u;
	if (Page >= PageCount)
		return NULL;

	if (ViewIndex == 0u)
	{
		*Count = Pages[Page].ElementCount;
		return Pages[Page].Elements;
	}
	if (ViewIndex == 1u && Pages_HasAlt(Page))
	{
		*Count = Pages[Page].AltElementCount;
		return Pages[Page].AltElements;
	}
	return NULL;
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
