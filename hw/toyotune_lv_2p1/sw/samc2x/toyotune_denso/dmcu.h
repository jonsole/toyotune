/*
 * dmcu.h
 *
 * Created: 06/12/2020 14:44:21
 *  Author: WinUser
 */ 


#ifndef DMCU_H_
#define DMCU_H_

#include <sam.h>
#include <stdint.h>
#include <stdbool.h>

extern void DMCU_Init(void);

extern void DMCU_ResetEnable(void);
extern void DMCU_ResetDisable(void);
extern bool DMCU_IsResetEnabled(void);
extern bool DMCU_IsHalted(void);

extern const uint8_t DMCU_Image[32768];

#define DMCU_REG_MEMC	(0x1E)
#define DMCU_REG_VER	(0x1F)

#endif /* DMCU_H_ */