/*
 * ui_gmeter.h
 *
 * The g-force meter's arithmetic: from the accelerometer's axes to the car's,
 * smoothed, with peaks held and a short trail. No hardware and no display
 * dependency, so it is host-tested; ui_gpage.c draws what this decides.
 *
 * THE MOUNT, AND ZEROING IT.
 *
 * The node sits in the dash facing the driver at whatever tilt the mount
 * gives it. Measured on the board, with the display facing its viewer:
 *
 *   sensor +X  towards the top of the display
 *   sensor +Y  towards the display's right  - the car's right
 *   sensor +Z  out of the back of the board - the car's front
 *
 * Zeroing stores the reading with the car standing level: that is gravity
 * plus the sensor's own offsets. Every later reading has it subtracted, which
 * leaves only the car's own acceleration, and "down" is its direction. The
 * lateral axis is then +Y made square to down, and the longitudinal one is
 * square to both - +Z for an upright mount - so a tilted mount reads true.
 * Before any zeroing the board is assumed to be upright.
 *
 * Signs: lateral is positive to the right, longitudinal positive when
 * accelerating. The dot moves the way the car is accelerating - right in a
 * right-hand bend, up when accelerating, down when braking.
 */
#ifndef UI_GMETER_H_
#define UI_GMETER_H_

#include <stdbool.h>
#include <stdint.h>

/* Readings, like the sensor's, in thousandths of a g. */
typedef struct
{
	int32_t X;
	int32_t Y;
	int32_t Z;
} GMeterMg_t;

/* The zeroing: the at-rest reading, gravity and offsets together. */
typedef struct
{
	GMeterMg_t Rest;
	bool Zeroed;			/* false: assumed upright, not measured */
} GMeterCal_t;

/* How many points the trail keeps, and how often it takes one: about two
   seconds of path, fading as it goes. It was a third of a second, which read
   as a smear rather than a record of where the car had been. */
#define GMETER_TRAIL_POINTS	(48u)
#define GMETER_TRAIL_STEP_US	(40000u)

/* How quickly the dot follows the sensor. Short enough to feel immediate,
   long enough that engine vibration does not shake it. */
#define GMETER_TAU_US		(60000u)

/* The slow average zeroing takes, so a single jolt while the button is held
   cannot become "level". */
#define GMETER_REST_TAU_US	(500000u)

typedef struct
{
	/* The car's acceleration, smoothed, in g. */
	float Lat;
	float Lon;

	/* Largest seen in each direction since the last reset, in g, all >= 0. */
	float PeakRight;
	float PeakLeft;
	float PeakAccel;
	float PeakBrake;

	/* Recent positions, oldest first, in g. */
	float TrailLat[GMETER_TRAIL_POINTS];
	float TrailLon[GMETER_TRAIL_POINTS];
	uint32_t TrailCount;
	uint32_t TrailDueUs;

	/* The raw reading, slowly averaged - what zeroing stores. */
	float SlowX, SlowY, SlowZ;
	bool Primed;
} GMeter_t;

/* The zeroing assumed before any has been done: the board upright, with
   gravity along +X. */
extern void GMeter_DefaultCal(GMeterCal_t *Cal);

extern void GMeter_Init(GMeter_t *G);

/* One sample, DtUs after the last. */
extern void GMeter_Update(GMeter_t *G, const GMeterCal_t *Cal, const GMeterMg_t *Raw,
                          uint32_t DtUs);

extern void GMeter_ResetPeaks(GMeter_t *G);

/* Take the present slow average as level. False, and Cal untouched, if there
   is not yet enough history to trust, or the reading is nowhere near 1 g -
   which would mean the car is not standing still. */
extern bool GMeter_Zero(const GMeter_t *G, GMeterCal_t *Cal);

#endif /* UI_GMETER_H_ */
