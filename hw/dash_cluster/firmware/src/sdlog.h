/*
 * sdlog.h
 *
 * ECU logging to the microSD card: a CSV file per drive, readable on any PC.
 *
 * WHEN IT LOGS. A file is opened when telemetry starts arriving and closed
 * when it stops or the card is pulled - so a drive is a file, and a node
 * sitting on a bench with no ECU writes nothing.
 *
 * WHICH CORE. Core 0, where the CAN traffic already lands. A card can stall
 * for tens of milliseconds while it does its own housekeeping, which on core
 * 0 delays the heartbeat and the console but NOT reception - that is
 * interrupt driven and keeps filling the store. On core 1 the same stall
 * would freeze the gauges.
 *
 * POWER CUT. A log is written as it goes, and the ignition can be switched
 * off at any moment, so the file is flushed every SDLOG_SYNC_MS - which
 * leaves at most that much of the drive unwritten, and keeps the card's
 * directory honest for everything before it. Nothing here delays the flush to
 * make the writing tidier; a log that loses its last second is far better
 * than one a PC will not open.
 */
#ifndef SDLOG_H_
#define SDLOG_H_

#include <stdbool.h>
#include <stdint.h>

/* A row every 50 ms. The fastest signals arrive every 20 ms, so this is not
   every value; it is enough to see a gearchange, and it keeps a long drive
   to a few hundred MB. */
#define SDLOG_PERIOD_MS		(50u)

/* Flushed to the card this often - the most a power cut can lose. */
#define SDLOG_SYNC_MS		(2000u)

/* Rows are gathered here and written in one go: a card writes in blocks, and
   a row at a time would be a block rewrite each. */
#define SDLOG_BUFFER		(4096u)

extern void SdLog_Init(void);

/* Call from core 0's loop. Opens, writes, flushes and closes as the link and
   the card come and go; never blocks waiting for either. */
extern void SdLog_Poll(uint32_t NowMs);

/* For the status line. */
extern bool SdLog_Active(void);
extern const char *SdLog_FileName(void);
extern uint32_t SdLog_Rows(void);
extern uint32_t SdLog_Bytes(void);
extern uint32_t SdLog_Errors(void);

#endif /* SDLOG_H_ */
