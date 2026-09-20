/*
 * test_logfmt.c - host tests for the log's lines.
 *
 * A log is looked at long after the drive, by which time nobody can tell a
 * held-over reading from a real one. So the rule these tests exist for is:
 * a value that was missing or stale is an EMPTY CELL, never the last value
 * seen. The rest is making sure a row lines up with its header, which is the
 * other way a log quietly lies.
 */

#include <stdio.h>
#include <string.h>

#include "logfmt.h"
#include "signal_store.h"

extern int LogFmtTests_Run(int *Checks, int *Failures);

static int Checks;
static int Failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		Checks++;                                                             \
		if (!(cond)) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
			printf(__VA_ARGS__);                                              \
			printf("\n");                                                     \
		}                                                                     \
	} while (0)


static uint32_t Commas(const char *S)
{
	uint32_t n = 0;

	while (*S != '\0')
	{
		if (*S == ',')
			n++;
		S++;
	}
	return n;
}


/* The text of column Index (0 is the time), or "" past the end. */
static const char *Cell(const char *Line, uint32_t Index, char *Out, uint32_t Size)
{
	uint32_t Field = 0;
	uint32_t At = 0;

	Out[0] = '\0';
	while (*Line != '\0' && *Line != '\n')
	{
		if (*Line == ',')
		{
			if (Field == Index)
				break;
			Field++;
			At = 0;
			Out[0] = '\0';
		}
		else if (Field == Index && At + 1u < Size)
		{
			Out[At++] = *Line;
			Out[At] = '\0';
		}
		Line++;
	}
	return Out;
}


/***************************************************************************************/
int LogFmtTests_Run(int *OutChecks, int *OutFailures)
{
	char Header[LOGFMT_LINE_MAX];
	char Row[LOGFMT_LINE_MAX];
	char Text[32];
	uint32_t Id, Columns;

	Checks = 0;
	Failures = 0;
	printf("log - the CSV a drive is written as\n");

	CHECK(LogFmt_Header(Header, sizeof(Header)) < sizeof(Header),
	      "the header fits in a line buffer");
	CHECK(strncmp(Header, "time_ms,", 8u) == 0, "it starts with the time: %.16s", Header);
	CHECK(strstr(Header, "RPM(rpm)") != NULL, "and names each column with its unit");
	CHECK(strstr(Header, "Lat g") == NULL,
	      "the accelerometer is not a column - it never reaches the store");
	CHECK(Header[strlen(Header) - 1u] == '\n', "and the line ends");

	Columns = Commas(Header);

	/* An empty store: every cell empty, and still the same number of them. */
	SignalStore_Init();
	CHECK(LogFmt_Row(Row, sizeof(Row), 0u, 1000u) < sizeof(Row), "a row fits too");
	CHECK(Commas(Row) == Columns, "a row has the header's columns: %lu against %lu",
	      (unsigned long)Commas(Row), (unsigned long)Columns);
	CHECK(strncmp(Row, "0,,", 3u) == 0, "nothing known yet reads as empty cells: %.12s", Row);

	/* A reading appears in its own column, in engineering units. */
	SignalStore_Set(SIGNAL_RPM, 3456, 1000u);
	SignalStore_Set(SIGNAL_ECT, 8850, 1000u);
	(void)LogFmt_Row(Row, sizeof(Row), 1234u, 1000u);
	CHECK(Commas(Row) == Columns, "still the header's columns");
	CHECK(strncmp(Row, "1234,", 5u) == 0, "the elapsed time leads the row: %.8s", Row);

	for (Id = 0, Columns = 0; Id < (uint32_t)SIGNAL_COUNT; Id++)
	{
		if (!LogFmt_Logged((SignalId_t)Id))
			continue;
		Columns++;				/* column 1 is the first signal */
		if ((SignalId_t)Id == SIGNAL_RPM)
			CHECK(strcmp(Cell(Row, Columns, Text, sizeof(Text)), "3456") == 0,
			      "the revs are in their column: %s", Text);
		if ((SignalId_t)Id == SIGNAL_ECT)
			CHECK(strcmp(Cell(Row, Columns, Text, sizeof(Text)), "88.50") == 0,
			      "the coolant in degrees: %s", Text);
	}

	/* THE ONE THAT MATTERS. Time passes and nothing new arrives: the cells
	   go empty rather than repeating the last reading for ever. */
	{
		uint32_t Late = 1000u;
		SignalReading_t R;

		do {
			Late += 100u;
			R = SignalStore_Get(SIGNAL_RPM, Late);
		} while (R.Fresh && Late < 61000u);

		CHECK(!R.Fresh, "the store calls the reading stale eventually");
		(void)LogFmt_Row(Row, sizeof(Row), Late - 1000u, Late);
		CHECK(strcmp(Cell(Row, 1u, Text, sizeof(Text)), "") == 0,
		      "a stale reading is an empty cell, not the last value: '%s'", Text);
	}

	/* A short buffer truncates rather than running off the end, and says so
	   by returning the length it wanted. */
	{
		char Small[24];
		uint32_t Wanted = LogFmt_Header(Small, sizeof(Small));

		CHECK(Wanted >= sizeof(Small), "a short buffer reports the full length");
		CHECK(strlen(Small) < sizeof(Small), "and stays inside itself");
	}

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
