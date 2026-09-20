/*
 * logfmt.c - the log's header and rows. See logfmt.h.
 */

#include "logfmt.h"

#include <stdio.h>
#include <string.h>

#include "signal_store.h"


/***************************************************************************************/
bool LogFmt_Logged(SignalId_t Signal)
{
	return Signal < SIGNAL_COUNT
	       && Signal != SIGNAL_G_LAT && Signal != SIGNAL_G_LON
	       && Signal != SIGNAL_CLOCK;
}


/***************************************************************************************/
/* Append what fits, and report the length the line would have had - so a
   caller can tell a truncated line from a complete one rather than writing
   half a row into a file. */
static uint32_t LogFmt_Append(char *Out, uint32_t Size, uint32_t At, const char *Text)
{
	uint32_t Len = (uint32_t)strlen(Text);
	uint32_t i;

	for (i = 0; i < Len && (At + i + 1u) < Size; i++)
		Out[At + i] = Text[i];

	if (Size > 0u)
		Out[(At + i < Size) ? (At + i) : (Size - 1u)] = '\0';

	return At + Len;
}


/***************************************************************************************/
uint32_t LogFmt_Header(char *Out, uint32_t Size)
{
	uint32_t At = 0;
	uint32_t Id;

	if (Size > 0u)
		Out[0] = '\0';

	At = LogFmt_Append(Out, Size, At, "time_ms");
	for (Id = 0; Id < (uint32_t)SIGNAL_COUNT; Id++)
	{
		const SignalDescriptor_t *D = &SignalDescriptors[Id];
		char Column[48];

		if (!LogFmt_Logged((SignalId_t)Id))
			continue;

		/* "Coolant(degC)", or just the name where there is no unit - a raw
		   count has none, and "TPS()" reads like a mistake. */
		if (D->Unit != NULL && D->Unit[0] != '\0')
			(void)snprintf(Column, sizeof(Column), ",%s(%s)", D->Name, D->Unit);
		else
			(void)snprintf(Column, sizeof(Column), ",%s", D->Name);
		At = LogFmt_Append(Out, Size, At, Column);
	}

	return LogFmt_Append(Out, Size, At, "\n");
}


/***************************************************************************************/
uint32_t LogFmt_Row(char *Out, uint32_t Size, uint32_t ElapsedMs, uint32_t NowMs)
{
	char Cell[32];
	uint32_t At = 0;
	uint32_t Id;

	if (Size > 0u)
		Out[0] = '\0';

	(void)snprintf(Cell, sizeof(Cell), "%lu", (unsigned long)ElapsedMs);
	At = LogFmt_Append(Out, Size, At, Cell);

	for (Id = 0; Id < (uint32_t)SIGNAL_COUNT; Id++)
	{
		SignalReading_t R;

		if (!LogFmt_Logged((SignalId_t)Id))
			continue;

		R = SignalStore_Get((SignalId_t)Id, NowMs);
		if (!R.Valid || !R.Fresh)
		{
			/* An empty cell: nothing was known. See the header comment. */
			At = LogFmt_Append(Out, Size, At, ",");
			continue;
		}

		Cell[0] = ',';
		(void)Signal_Format((SignalId_t)Id, R.Value, &Cell[1], sizeof(Cell) - 1u);
		At = LogFmt_Append(Out, Size, At, Cell);
	}

	return LogFmt_Append(Out, Size, At, "\n");
}
