/*
 * logfmt.h
 *
 * The log's lines: a header naming every column, and a row of readings.
 *
 * Comma separated, in engineering units, one column per signal the bus
 * carries - so a log opens in a spreadsheet with no tool of ours in the way,
 * which is the whole point of putting it on a FAT card.
 *
 * COLUMNS COME FROM THE SIGNAL TABLE, not from a list kept here. A signal
 * added to signals.h appears in the log by itself, and the header says what
 * each column is, so an old log stays readable when the columns change.
 *
 * A READING THAT IS MISSING OR STALE IS AN EMPTY CELL - never the last value
 * held over. A spreadsheet plots a gap; a held value would be read as a
 * measurement, which is the same rule the gauges follow.
 *
 * No files and no card here, so both are testable on a host. sdlog.c does the
 * writing.
 */
#ifndef LOGFMT_H_
#define LOGFMT_H_

#include <stdbool.h>
#include <stdint.h>

#include "signals.h"

/* Enough for the header or a row, with every signal present. */
#define LOGFMT_LINE_MAX		(768u)

/* "time_ms,RPM(rpm),Boost(kPa),..." with a newline. Returns the length. */
extern uint32_t LogFmt_Header(char *Out, uint32_t Size);

/* One row: the time in milliseconds since the log started, then every
   signal's reading as of NowMs. Returns the length. */
extern uint32_t LogFmt_Row(char *Out, uint32_t Size, uint32_t ElapsedMs, uint32_t NowMs);

/* Which signals are columns: those the bus carries. The accelerometer's and
   the clock are read on the display core and never enter the store. */
extern bool LogFmt_Logged(SignalId_t Signal);

#endif /* LOGFMT_H_ */
