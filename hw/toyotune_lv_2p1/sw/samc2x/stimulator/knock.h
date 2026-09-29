/*
 * knock.h
 *
 * Created: 09/02/2019 11:34:42
 *  Author: WinUser
 */ 


#ifndef KNOCK_H_
#define KNOCK_H_

/* Burst amplitude, 0 to 255. Peak deviation from mid-scale is
   (Knock_Severity / 256) of half the DAC range. Zero produces a flat
   mid-scale burst and no ping at all. A plain global so it can be poked
   over SWD while the stimulator runs - see sw/python/set_rpm.py for the
   same trick applied to VRG_Rpm. */
extern uint8_t Knock_Severity;

/* The engine's background vibration, on the same scale as Knock_Severity,
   present all the time the engine turns rather than once per ignition. A
   real knock sensor never outputs silence, and the ECU treats a silent one as
   dead: with the engine warm enough for knock control it sets code 52 and
   holds its fail-safe maximum retard. Also pokeable over SWD; a change takes
   effect at the next ignition. */
extern uint8_t Knock_Noise;

/* Where the knock lands: this many degrees of crank after the IGT edge (the
   spark), converted to time at the current engine speed on every ignition.
   Real knock comes after TDC, when cylinder pressure peaks, and the knock chip
   only listens in a window there - a burst at the spark itself arrives before
   it is listening. Default 25, about 15 degrees ATDC with 10 degrees advance. */
extern uint8_t Knock_DelayDeg;

/* How long the knock rings, in samples at 53.6 kHz (53.6 per ms), up to 512.
   The next ignition cuts it short at high revs, as it would in an engine. */
extern uint16_t Knock_RingSamples;

/* How fast the ring dies: multiplied by Knock_Decay/256 each sample. 252 (the
   default) is a 1.2 ms time constant, 254 about 2.4 ms, 255 about 4.8 ms.
   Lengthening Knock_RingSamples alone does little - past about three time
   constants the ring is already down to a few percent. */
extern uint8_t Knock_Decay;

/* Knock on Knock_Count consecutive ignitions out of every Knock_Every, and
   background only on the rest (default 4 in 20). A knock chip learns its
   background from what it hears, so a burst on every ignition becomes the
   background and reads as no knock at any severity. Knock_Every of 0 or 1
   knocks on every ignition, as before. */
extern uint8_t Knock_Count;
extern uint8_t Knock_Every;

void Knock_Init(void);
void Knock_Trigger(uint8_t Severity);


#endif /* KNOCK_H_ */