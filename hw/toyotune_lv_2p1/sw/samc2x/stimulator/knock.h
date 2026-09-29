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

void Knock_Init(void);
void Knock_Trigger(uint8_t Severity);


#endif /* KNOCK_H_ */