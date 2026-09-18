/*
 * clock_link.c - the time announced on the bus. See clock_link.h.
 */

#include "clock_link.h"

#include <string.h>

/* Written by core 0 from the CAN callback, read by core 1. The flag is set
   last and cleared first, so core 1 cannot act on a half-written time; a
   single byte needs no more than that, and losing one announcement to a race
   would cost a second. */
static volatile uint8_t		Pending[3];
static volatile bool		Waiting;
static uint32_t			Accepted;
static uint32_t			Rejected;


/***************************************************************************************/
void ClockLink_Init(void)
{
	Waiting = false;
	Accepted = 0u;
	Rejected = 0u;
}


/***************************************************************************************/
bool ClockLink_Handle(uint16_t Id, const uint8_t *Data, uint8_t Length)
{
	if (Id != CLOCK_LINK_TIME_ID)
		return false;

	/* Every reason to reject is counted, so an announcer that is nearly right
	   shows up as a number rather than as a clock that never sets itself. */
	if (Length < 4u
	    || (Data[0] & CLOCK_LINK_FLAG_VALID) == 0u
	    || Data[1] > 23u || Data[2] > 59u || Data[3] > 59u)
	{
		Rejected++;
		return true;
	}

	Pending[0] = Data[1];
	Pending[1] = Data[2];
	Pending[2] = Data[3];
	Waiting = true;
	Accepted++;
	return true;
}


/***************************************************************************************/
bool ClockLink_Take(RtcTime_t *Out)
{
	if (!Waiting)
		return false;

	Waiting = false;
	Out->Hours = Pending[0];
	Out->Minutes = Pending[1];
	Out->Seconds = Pending[2];
	return true;
}


/***************************************************************************************/
uint32_t ClockLink_Accepted(void)	{ return Accepted; }
uint32_t ClockLink_Rejected(void)	{ return Rejected; }
