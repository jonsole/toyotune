/*
 * signals.c
 *
 * The signal descriptor table, and integer formatting for display.
 *
 * Ranges here are display ranges - what a gauge should sweep between - not
 * validity limits. A value outside them is still shown; clamping a reading
 * because it looked wrong is how a fault gets hidden.
 */

#include <stdio.h>
#include <string.h>

#include "signals.h"

#define FAST_MS		(20)
#define MEDIUM_MS	(100)
#define SLOW_MS		(500)
#define INFO_MS		(1000)

const SignalDescriptor_t SignalDescriptors[SIGNAL_COUNT] =
{
	[SIGNAL_RPM]           = { "RPM",     "rpm",  0,     0, 8000,  FAST_MS },
	[SIGNAL_TPS_RAW]       = { "TPS",     "",     0,     0, 65535, FAST_MS },
	[SIGNAL_MAP]           = { "Boost",   "kPa",  1,     0, 2500,  FAST_MS },
	[SIGNAL_INJ_PW]        = { "InjPW",   "us",   0,     0, 25000, FAST_MS },

	[SIGNAL_ECT]           = { "Coolant", "degC", 2, -4000, 12000, MEDIUM_MS },
	[SIGNAL_THA]           = { "Intake",  "degC", 2, -4000, 12000, MEDIUM_MS },
	[SIGNAL_THAM]          = { "Manifold","degC", 2, -4000, 12000, MEDIUM_MS },
	[SIGNAL_BATTERY]       = { "Battery", "V",    2,     0,  1800, MEDIUM_MS },

	[SIGNAL_INJ_DUTY]      = { "Duty",    "%",    2,     0, 10000, MEDIUM_MS },
	[SIGNAL_KNOCK_RETARD]  = { "Knock",   "deg",  2,     0,  2000, MEDIUM_MS },
	[SIGNAL_IGN_TIMING_RAW]= { "IgnRaw",  "",     0,     0,   255, MEDIUM_MS },
	[SIGNAL_ISCV_DUTY_RAW] = { "ISCV",    "",     0,     0,   255, MEDIUM_MS },
	[SIGNAL_LAMBDA_RAW]    = { "O2raw",   "",     0,     0,   255, MEDIUM_MS },
	[SIGNAL_PW_LOOP_MODE]  = { "Loop",    "",     0,     0,   255, MEDIUM_MS },

	[SIGNAL_KNOCK_CYL1]    = { "Knock1",  "deg",  2,     0,  2000, MEDIUM_MS },
	[SIGNAL_KNOCK_CYL2]    = { "Knock2",  "deg",  2,     0,  2000, MEDIUM_MS },
	[SIGNAL_KNOCK_CYL3]    = { "Knock3",  "deg",  2,     0,  2000, MEDIUM_MS },
	[SIGNAL_LAMBDA_TRIM_RAW]={ "O2trim",  "",     0,     0,   255, MEDIUM_MS },
	[SIGNAL_MAX_RETARD_RAW]= { "MaxRet",  "",     0,     0,   255, MEDIUM_MS },

	[SIGNAL_NV_TRIM_PIM_RAW]={ "TrimMAP", "",     0,     0,   255, SLOW_MS },
	[SIGNAL_NV_TRIM_O2_RAW]= { "TrimO2",  "",     0,     0,   255, SLOW_MS },
	[SIGNAL_FUEL_TRIM_RAW] = { "FuelTrim","",     0,     0,   255, SLOW_MS },
	[SIGNAL_ERROR_FLAGS1]  = { "Err1",    "",     0,     0,   255, SLOW_MS },
	[SIGNAL_ERROR_FLAGS2]  = { "Err2",    "",     0,     0,   255, SLOW_MS },
	[SIGNAL_FLAGS46]       = { "Flags46", "",     0,     0,   255, SLOW_MS },
	[SIGNAL_FLAGS1]        = { "Flags1",  "",     0,     0,   255, SLOW_MS },
	[SIGNAL_LIMITER_FLAGS] = { "Limiter", "",     0,     0,   255, SLOW_MS },

	[SIGNAL_PROTOCOL_VERSION]  = { "Proto",  "", 0, 0,   255, INFO_MS },
	[SIGNAL_ECU_FAMILY]        = { "Family", "", 0, 0,   255, INFO_MS },
	[SIGNAL_CPU_INDEX]         = { "CPU",    "", 0, 0,   255, INFO_MS },
	[SIGNAL_TX_DROPPED]        = { "TxDrop", "", 0, 0, 65535, INFO_MS },
	[SIGNAL_BUS_OFF_RECOVERIES]= { "BusOff", "", 0, 0, 65535, INFO_MS }
};


/***************************************************************************************/
/* Insert the decimal point by hand rather than dividing into a float.
 *
 * A gauge redraws tens of times a second on a core that is also servicing a
 * CAN interrupt; pulling in soft-float formatting for it would be a poor
 * trade, and integer division here is exact where a float would round twice -
 * once into the float and again in the print.
 *
 * Negative values are handled by formatting the magnitude and prefixing the
 * sign, so -1.2 does not come out as "-0.-2" from a truncating divide.
 */
const char *Signal_Format(SignalId_t Id, int32_t Value, char *Out, uint32_t OutSize)
{
	static const int32_t Pow10[] = { 1, 10, 100, 1000 };
	uint8_t Decimals;
	int32_t Scale, Whole, Frac;
	uint32_t Magnitude;
	const char *Sign = "";

	if (Out == NULL || OutSize == 0)
		return Out;

	if (Id >= SIGNAL_COUNT)
	{
		snprintf(Out, OutSize, "?");
		return Out;
	}

	Decimals = SignalDescriptors[Id].Decimals;
	if (Decimals > 3)
		Decimals = 3;
	Scale = Pow10[Decimals];

	if (Value < 0)
	{
		Sign = "-";
		Magnitude = (uint32_t)(-(int64_t)Value);
	}
	else
	{
		Magnitude = (uint32_t)Value;
	}

	Whole = (int32_t)(Magnitude / (uint32_t)Scale);
	Frac = (int32_t)(Magnitude % (uint32_t)Scale);

	if (Decimals == 0)
		snprintf(Out, OutSize, "%s%ld", Sign, (long)Whole);
	else
		snprintf(Out, OutSize, "%s%ld.%0*ld", Sign, (long)Whole,
		         (int)Decimals, (long)Frac);

	return Out;
}
