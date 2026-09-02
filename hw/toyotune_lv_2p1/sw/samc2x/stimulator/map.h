/*
 * map.h
 *
 * Manifold absolute pressure output, driven out through the SPI DAC.
 */


#ifndef MAP_H_
#define MAP_H_

void MAP_Init(void);
void MAP_Set(uint16_t Millibar);


#endif /* MAP_H_ */
