/*
 * ui_model.c
 *
 * Turning a stored signal into something a widget can draw.
 */

#include <string.h>

#include "signal_store.h"
#include "ui_model.h"


/***************************************************************************************/
bool UiModel_SignalWarning(SignalId_t Signal, int32_t Value)
{
	switch (Signal)
	{
	case SIGNAL_ECT:
		return Value >= UI_WARN_ECT_C100;

	case SIGNAL_BATTERY:
		/* Low, not high: a flat battery is the failure a driver can act on,
		   and an overvoltage reads as a regulator fault the ECU flags anyway. */
		return Value <= UI_WARN_BATTERY_LOW_V100;

	case SIGNAL_INJ_DUTY:
		return Value >= UI_WARN_INJ_DUTY_PCT100;

	case SIGNAL_KNOCK_RETARD:
	case SIGNAL_KNOCK_CYL1:
	case SIGNAL_KNOCK_CYL2:
	case SIGNAL_KNOCK_CYL3:
		return Value >= UI_WARN_KNOCK_DEG100;

	case SIGNAL_ERROR_FLAGS1:
	case SIGNAL_ERROR_FLAGS2:
	case SIGNAL_LIMITER_FLAGS:
		return Value != 0;

	default:
		return false;
	}
}


/***************************************************************************************/
/* Map a value onto 0..1000 of the element's range.
 *
 * Done in 64-bit because the intermediate multiply overflows 32 bits for a
 * wide range - Rpm spans 8000 and InjPw 25000, and (Value - Min) * 1000 is
 * already 25 million before the divide. Clamped rather than wrapped: a needle
 * that wraps past full scale back to zero reads as an idling engine.
 */
static uint16_t UiModel_Position(int32_t Value, int32_t Min, int32_t Max,
                                 bool *OffScale)
{
	int64_t Span, Offset, Scaled;

	*OffScale = false;

	if (Max <= Min)
		return 0;

	if (Value <= Min)
	{
		*OffScale = (Value < Min);
		return 0;
	}
	if (Value >= Max)
	{
		*OffScale = (Value > Max);
		return UI_POSITION_MAX;
	}

	Span = (int64_t)Max - (int64_t)Min;
	Offset = (int64_t)Value - (int64_t)Min;
	Scaled = (Offset * UI_POSITION_MAX) / Span;

	return (uint16_t)Scaled;
}


/***************************************************************************************/
UiWidget_t UiModel_Widget(const FaceElement_t *Element, uint32_t NowMs)
{
	UiWidget_t W;
	SignalReading_t R;
	const SignalDescriptor_t *D;

	memset(&W, 0, sizeof(W));

	if (Element == NULL || Element->Signal >= SIGNAL_COUNT)
	{
		W.State = UI_STATE_NODATA;
		W.Unit = "";
		W.Label = "?";
		strncpy(W.Text, "--", sizeof(W.Text) - 1);
		return W;
	}

	D = &SignalDescriptors[Element->Signal];
	W.Label = D->Name;
	W.Unit = D->Unit;

	R = SignalStore_Get(Element->Signal, NowMs);
	W.Value = R.Value;

	if (!R.Valid)
	{
		/* Nothing has ever arrived. The needle sits at zero because it has to
		   sit somewhere, but the text says so - a gauge reading zero and a
		   gauge with no data must not look the same. */
		W.State = UI_STATE_NODATA;
		W.Position = 0;
		strncpy(W.Text, "--", sizeof(W.Text) - 1);
		return W;
	}

	W.Position = UiModel_Position(R.Value, Element->Min, Element->Max, &W.OffScale);
	Signal_Format(Element->Signal, R.Value, W.Text, sizeof(W.Text));

	/* Staleness outranks a warning band. A value that is too old to trust
	   should not also be asserting that the engine is too hot - the reading
	   it is asserting from may be minutes out of date. */
	if (!R.Fresh)
		W.State = UI_STATE_STALE;
	else if (UiModel_SignalWarning(Element->Signal, R.Value))
		W.State = UI_STATE_WARNING;
	else
		W.State = UI_STATE_NORMAL;

	return W;
}
