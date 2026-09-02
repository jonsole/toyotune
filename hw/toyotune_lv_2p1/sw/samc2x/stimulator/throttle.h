/*
 * throttle.h
 *
 * Throttle position (VTA) output, driven out through the SPI DAC, plus the
 * idle contact. Value is a percentage, 0 to 100.
 */


#ifndef THROTTLE_H_
#define THROTTLE_H_

void Throttle_Init(void);
void Throttle_Set(uint8_t Value);


#endif /* THROTTLE_H_ */
