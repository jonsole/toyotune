/*
 * audio.h
 *
 * The speaker: an ES8311 codec, I2S out of a PIO block, and a pair of DMA
 * buffers that never stop.
 *
 * WHY THE STREAM NEVER STOPS
 *
 * The obvious design is to start the DMA when a warning begins and stop it
 * when the warning ends. Do that and the state machine stalls on `pull` with
 * the data line wherever the last bit left it, which the codec reads as a
 * held DC level into the speaker. So the stream runs from boot to power-off
 * and silence is a buffer of zeros - a fixed 96 KB/s of DMA that costs
 * nothing measurable and removes a whole class of click, pop and buzz.
 *
 * WHICH CORE
 *
 * All of it runs on core 1, and that is deliberate twice over. The codec is
 * on the I2C bus that the touch panel, the IMU and the RTC are on, and that
 * bus has one owner; and core 0 is where can2040 wants the interrupt latency,
 * so the buffer-refill interrupt is better anywhere else. Audio_Init() must
 * therefore be called from core 1, after Panel_Init() has brought the bus up.
 *
 * Audio_Warn() is the whole interface: say what should be sounding, as often
 * as you like. The pattern itself runs in the DMA interrupt off tone.c, so a
 * caller that is late with a frame does not stretch a beep.
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "tone.h"

/* Codec volume at boot, on Waveshare's scale - which is 0.5 dB a step
   rather than a percentage of loudness. 75 is 0 dB, the loudest setting that
   cannot clip a full-scale tone; above it is digital gain. Below it, every
   point is about 1.3 dB quieter, so 60 is about -20 dB. 55 was inaudible and
   70 audible but quiet, before the tone itself was raised to full scale. */
#define AUDIO_VOLUME_PCT	(75u)

/* Brings up the codec and starts the silent stream. False if the codec did
   not answer on I2C, in which case nothing else here does anything - a node
   with no speaker is a node that draws gauges, not a node that fails. */
extern bool Audio_Init(void);

extern bool Audio_Present(void);

/* What should be sounding. Cheap and idempotent: the same sound repeated is
   ignored, so this belongs in the main loop rather than behind a change
   test. */
extern void Audio_Warn(ToneId_t Id);

/* What was last asked for, for the status line. */
extern ToneId_t Audio_Sounding(void);

/* Codec volume, percent. For the console - the car wants one setting and the
   bench another. */
extern void Audio_SetVolume(uint32_t Percent);
extern uint32_t Audio_Volume(void);

/* For the status line: how many buffers have gone out, and how many times the
   refill arrived too late and the DMA replayed a stale buffer. A non-zero
   second number is audible as a stutter and means something on core 1 blocked
   for longer than a buffer - see AUDIO_BUFFER_MS. */
extern uint32_t Audio_Buffers(void);
extern uint32_t Audio_Underruns(void);

/* What the codec answered when asked who it was, for the bench. */
extern uint16_t Audio_ChipId(void);

#endif /* AUDIO_H_ */
