/*
 * diag_can.h
 *
 * CAN command interface to the Denso diagnostic link: one command frame in,
 * one response frame out. See diag_can.c for the frame layout.
 */

#ifndef DIAG_CAN_H_
#define DIAG_CAN_H_

#include <stdint.h>

void DiagCan_Init(void);

#endif /* DIAG_CAN_H_ */
