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

void Knock_Init(void);
void Knock_Trigger(uint8_t Severity);


#endif /* KNOCK_H_ */