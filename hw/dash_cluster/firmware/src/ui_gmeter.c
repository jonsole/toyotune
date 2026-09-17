/*
 * ui_gmeter.c - the g-force meter's arithmetic. See ui_gmeter.h.
 */

#include "ui_gmeter.h"

#include <math.h>
#include <string.h>

/* A zeroing reading this far from 1 g is not a car standing still. */
#define GMETER_ZERO_MIN_MG	(850.0f)
#define GMETER_ZERO_MAX_MG	(1150.0f)


/***************************************************************************************/
void GMeter_DefaultCal(GMeterCal_t *Cal)
{
	Cal->Rest.X = 1000;
	Cal->Rest.Y = 0;
	Cal->Rest.Z = 0;
	Cal->Zeroed = false;
}


/***************************************************************************************/
void GMeter_Init(GMeter_t *G)
{
	memset(G, 0, sizeof(*G));
}


/***************************************************************************************/
void GMeter_ResetPeaks(GMeter_t *G)
{
	G->PeakRight = 0.0f;
	G->PeakLeft = 0.0f;
	G->PeakAccel = 0.0f;
	G->PeakBrake = 0.0f;
}


/***************************************************************************************/
static float GMeter_Dot(const float *A, const float *B)
{
	return (A[0] * B[0]) + (A[1] * B[1]) + (A[2] * B[2]);
}

/* V less its component along unit vector U, then made unit length. Returns
   false if nothing is left - V lies along U. */
static bool GMeter_SquareTo(float *V, const float *U)
{
	float D = GMeter_Dot(V, U);
	float L;
	int i;

	for (i = 0; i < 3; i++)
		V[i] -= D * U[i];

	L = sqrtf(GMeter_Dot(V, V));
	if (L < 1e-3f)
		return false;
	for (i = 0; i < 3; i++)
		V[i] /= L;
	return true;
}


/***************************************************************************************/
void GMeter_Update(GMeter_t *G, const GMeterCal_t *Cal, const GMeterMg_t *Raw,
                   uint32_t DtUs)
{
	float Down[3], Right[3] = { 0.0f, 1.0f, 0.0f }, Ahead[3];
	float Lin[3], L, Lat, Lon, A, Slow;
	int i;

	/* The slow average zeroing will take. Seeded from the first sample so it
	   does not have to climb from zero. */
	Slow = (float)DtUs / ((float)GMETER_REST_TAU_US + (float)DtUs);
	if (!G->Primed)
	{
		G->SlowX = (float)Raw->X;
		G->SlowY = (float)Raw->Y;
		G->SlowZ = (float)Raw->Z;
		G->Primed = true;
	}
	else
	{
		G->SlowX += ((float)Raw->X - G->SlowX) * Slow;
		G->SlowY += ((float)Raw->Y - G->SlowY) * Slow;
		G->SlowZ += ((float)Raw->Z - G->SlowZ) * Slow;
	}

	/* Down, from the zeroing: the direction the at-rest reading points in,
	   reversed - an accelerometer at rest reads upward. */
	Down[0] = -(float)Cal->Rest.X;
	Down[1] = -(float)Cal->Rest.Y;
	Down[2] = -(float)Cal->Rest.Z;
	L = sqrtf(GMeter_Dot(Down, Down));
	if (L < 1.0f)
		return;
	for (i = 0; i < 3; i++)
		Down[i] /= L;

	/* The car's right: the display's right, made square to down. Then ahead
	   is right x down, which is square to both by construction - and is the
	   sensor's +Z for an upright mount, the true ahead for a tilted one, and
	   the top of the display for a board lying flat on the bench, where +Z
	   points straight down and could not be squared to it at all. */
	if (!GMeter_SquareTo(Right, Down))
		return;
	Ahead[0] = (Right[1] * Down[2]) - (Right[2] * Down[1]);
	Ahead[1] = (Right[2] * Down[0]) - (Right[0] * Down[2]);
	Ahead[2] = (Right[0] * Down[1]) - (Right[1] * Down[0]);

	/* The car's own acceleration: the reading less the at-rest one. */
	Lin[0] = (float)(Raw->X - Cal->Rest.X);
	Lin[1] = (float)(Raw->Y - Cal->Rest.Y);
	Lin[2] = (float)(Raw->Z - Cal->Rest.Z);

	Lat = GMeter_Dot(Lin, Right) / 1000.0f;
	Lon = GMeter_Dot(Lin, Ahead) / 1000.0f;

	A = (float)DtUs / ((float)GMETER_TAU_US + (float)DtUs);
	G->Lat += (Lat - G->Lat) * A;
	G->Lon += (Lon - G->Lon) * A;

	if (G->Lat > G->PeakRight)
		G->PeakRight = G->Lat;
	if (-G->Lat > G->PeakLeft)
		G->PeakLeft = -G->Lat;
	if (G->Lon > G->PeakAccel)
		G->PeakAccel = G->Lon;
	if (-G->Lon > G->PeakBrake)
		G->PeakBrake = -G->Lon;

	/* The trail: a point every TRAIL_STEP, oldest dropped. */
	if (G->TrailDueUs <= DtUs)
	{
		if (G->TrailCount == GMETER_TRAIL_POINTS)
		{
			memmove(G->TrailLat, G->TrailLat + 1, sizeof(G->TrailLat[0]) * (GMETER_TRAIL_POINTS - 1u));
			memmove(G->TrailLon, G->TrailLon + 1, sizeof(G->TrailLon[0]) * (GMETER_TRAIL_POINTS - 1u));
			G->TrailCount--;
		}
		G->TrailLat[G->TrailCount] = G->Lat;
		G->TrailLon[G->TrailCount] = G->Lon;
		G->TrailCount++;
		G->TrailDueUs = GMETER_TRAIL_STEP_US;
	}
	else
	{
		G->TrailDueUs -= DtUs;
	}
}


/***************************************************************************************/
bool GMeter_Zero(const GMeter_t *G, GMeterCal_t *Cal)
{
	float Mag;

	if (!G->Primed)
		return false;

	Mag = sqrtf((G->SlowX * G->SlowX) + (G->SlowY * G->SlowY) + (G->SlowZ * G->SlowZ));
	if (Mag < GMETER_ZERO_MIN_MG || Mag > GMETER_ZERO_MAX_MG)
		return false;

	Cal->Rest.X = (int32_t)lroundf(G->SlowX);
	Cal->Rest.Y = (int32_t)lroundf(G->SlowY);
	Cal->Rest.Z = (int32_t)lroundf(G->SlowZ);
	Cal->Zeroed = true;
	return true;
}
