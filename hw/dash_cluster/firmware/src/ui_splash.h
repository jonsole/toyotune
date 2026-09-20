/*
 * ui_splash.h
 *
 * The power-on splash on the glass: the Toyota emblem fading in and out, then
 * this node's letter of the MR2 badge - M on node 0, R on node 1, 2 on node 2.
 * The timeline is splash_seq.c; the images are dash_splash.c.
 *
 * HOW IT FADES. Each image is drawn into the back buffer once and then sent
 * every frame through a palette scaled to that frame's brightness - so a fade
 * redraws nothing, and costs a palette of 256 entries and one push of the
 * image's rectangle a frame.
 *
 * It uses a surface the gauges are about to overwrite, so it costs no RAM,
 * and CAN runs on the other core throughout - the gauges come up on live
 * values, not on "--".
 */
#ifndef UI_SPLASH_H_
#define UI_SPLASH_H_

#include <stdbool.h>
#include <stdint.h>

/* Run the splash to the end, on core 1, after Panel_Init() and UiDraw_Init().
   Leaves the surface black. Returns false if it was cut short: by a touch -
   the driver wants the gauges now - or by a fault, which must never wait
   behind a logo. */
extern bool UiSplash_Run(uint8_t Surface, uint8_t NodeId);

#endif /* UI_SPLASH_H_ */
