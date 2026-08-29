#ifndef __MODE_H_
#define __MODE_H_

#define MODE_NORMAL			(0x01) /* Normal operation mode, Denso MCUs run from Flash on LV boards/SRAM on Gapfiller board */
#define MODE_NORMAL_SRAM	(0x02) /* Normal operation mode, Denso MCUs run from SRAM on USB board */
#define MODE_PROGRAM		(0x03) /* Program mode, Denso MCUs halted.  AVR programs Flash on LV boards/SRAM on Gapfiller board */
#define MODE_DFU			(0x04) /* Upgrade mode, reboots into bootloader */
#define MODE_LOW_POWER		(0x80)
#define MODE_PROGRAM_TEST	(0xFF) /* Force program mode, same as MODE_PROGRAM but bypasses all checks */

extern void Mode_Init(void);
extern void Mode_Set(uint8_t Mode);
extern uint8_t Mode_Get(void);
extern void Mode_Push(uint8_t Mode);
extern void Mode_Pop(void);

#endif
