#include "toyotune_avr.h"

static uint8_t Mode = 0xFF;

void Mode_Init(void)
{
	/* Power up external bus */
	Mcu_BusPower(1);
	
	/* Move to normal mode */
#ifdef IS_TOYOTUNE_BOARD
	Mode_Set(MODE_NORMAL);
#endif
#ifdef IS_GAPFILLER_BOARD
	Mode_Set(MODE_NORMAL_SRAM);
#endif
}

void Mode_Set(uint8_t NewMode)
{
	/* Check new mode is different to current mode */
	if (NewMode != Mode)
	{		
		/* Check if new mode is available */
		switch (NewMode)
		{
			case MODE_NORMAL:
			{
				/* Nothing to check */
#ifdef HAVE_SRAM
				/* Gapfiller always in SRAM */
				NewMode =  MODE_NORMAL_SRAM;
				Debug_Info(PSTR("Mode not supported. Forcing MODE_NORMAL_SRAM\n"));
#endif				
			}
			break;

			case MODE_LOW_POWER:
			{		
#ifdef HAVE_SRAM
				/* Not supported by Gapfiller */
				Debug_Info(PSTR("Mode not supported.\n"));
				return;
#endif				
			}
			break;				

			/* Normal execution from SRAM mode, Denso MCUs execute code from SRAM on ToyoTune USB RT board */
			case MODE_NORMAL_SRAM:
			{
#ifdef HAVE_PAGEABLE_SRAM
				/* Confirm all MCUs are not running */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
				{
					/* Return immediately if MCUs are running */
					if (Mcu_IsRunning(Mcu))
					{
						Debug_Info(PSTR("Cannot enter SRAM Mode - MCU %d still running\n"), Mcu);
						return;
					}
				}
				/* Check flash interface to MCUs */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
				{
					/* Check if MCU is installed */
					if (Mcu_Type(Mcu))
					{
						/* Test flash interface on MCU board, exit if test fails */
						if (!Hal_MemorySramTest(Mcu))
						{
							Debug_Info(PSTR("Cannot enter SRAM Mode - MCU %d failed SRAM test\n"), Mcu);
							return;
						}
					}
				}
#else 
#ifdef HAVE_SRAM
				/* nothing to check on Gapfiller always in SRAM */
#else
				/* Not implemented so return now */
				return;
#endif
#endif
			}
			break;			

			/* DFU Programming mode, Denso MCUs must be in reset to enter this mode */
			case MODE_DFU:
			{
				/* Confirm all MCUs are not running */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
				{
					/* Return immediately if MCUs are running */
					if (Mcu_IsRunning(Mcu))
					{
						Debug_Info(PSTR("Cannot enter DFU Program Mode - MCU %d still running\n"), Mcu);
						return;
					}
				}

				/* Assert reset, force everything to stay not running */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
					Mcu_ResetEnable(Mcu, 1);
			}
			break;			

			case MODE_PROGRAM:
			{
				/* Check all MCUs are not running */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
				{
					/* Return immediately if MCUs are running */
					if (Mcu_IsRunning(Mcu))
					{
						Debug_Info(PSTR("Cannot enter Program Mode - MCU %d still running\n"), Mcu);
						return;
					}
				}

				/* Check flash interface to MCUs */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
				{
					/* Check if MCU is installed */
					if (Mcu_Type(Mcu))
					{
						/* Test flash interface on MCU board, exit if test fails */
						if (!Mcu_FlashTest(Mcu))
						{
							Debug_Info(PSTR("Cannot enter Program Mode - MCU %d failed flash test\n"), Mcu);
							return;
						}
					}
				}
			}
			break;			

			case MODE_PROGRAM_TEST:
			{
				/* Skip checks and enter MODE_PROGRAM */
				NewMode = MODE_PROGRAM;
			}
			break;

			default:
			{
				/* Unknown mode */
				Debug_Error(PSTR("Error: Unknown mode %d\n"), NewMode);
				return;
			}
			break;
		}

		Debug_Test(PSTR("Exiting mode %d\n"), Mode);

		/* Handle exiting current mode */
		switch (Mode)
		{
			case MODE_NORMAL:
			{
			}
			break;

			case MODE_NORMAL_SRAM:
			{
#ifdef HAVE_PAGEABLE_SRAM
				/* Page out SRAM */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
					Mcu_SramDisable(Mcu);
#endif
			}
			break;
			
			case MODE_LOW_POWER:
			{		
				/* Power up external bus */
				Mcu_BusPower(1);
			}
			break;				

			case MODE_PROGRAM:
			{
				/* Releases reset */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
					Mcu_ResetEnable(Mcu, 0);
			}
			break;			
		}
		Debug_Test(PSTR("Entering mode %d\n"), NewMode);

		/* Handle entering new mode */
		switch (NewMode)
		{
			/* Normal execution mode, Denso MCUs execute code from flash on LV boards or GapFiller board */
			case MODE_NORMAL:
			{
//				Led_SetPattern(Ui_NormalModePattern, LED_PRI_LOW);

				/* Move to normal mode */
				Mode = MODE_NORMAL;
			}
			break;

			/* SRAM execution mode, Denso MCUs execute code from SRAM on LV boards */
			case MODE_NORMAL_SRAM:
			{
//				Led_SetPattern(Ui_NormalSramModePattern, LED_PRI_LOW);

#ifdef IS_TOYOTUNE_BOARD
#if TOYOTUNE_VER >= 2
				/* Page in SRAM */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
					Mcu_SramEnable(Mcu);
#endif
#endif
				/* Mode to SRAM mode */
				Mode = MODE_NORMAL_SRAM;
			}
			break;
						
			case MODE_LOW_POWER:
			{		
//				Led_SetPattern(Ui_LowPowerModePattern, LED_PRI_LOW);

				/* Power down external bus */
				Mcu_BusPower(0);

				/* Make sure sleep mode is set */
				Hal_SleepSetMode(HAL_SLEEP_POWER_DOWN);
				
				/* Move to low power mode */
				Mode = MODE_LOW_POWER;				
			}
			break;				

			/* Programming mode, Denso MCUs must be in reset to enter this mode */
			case MODE_PROGRAM:
			{
				Led_SetPattern(Ui_ProgramModePattern, LED_PRI_LOW);

				/* Assert reset, force everything to stay not running */
				for (int Mcu = 0; Mcu < MCU_MAX; Mcu++)
					Mcu_ResetEnable(Mcu, 1);

				/* Move to programming mode */
				Mode = MODE_PROGRAM;
			}
			break;			

			/* Device firmware upgrade mode, update AVR firmware. Force board to reboot into LUFA HID bootloader */
			case MODE_DFU:
			{
				/* Confirm start of DFU mode by activating LEDs */
				Led_SetPattern(Ui_DfuModePattern, LED_PRI_LOW);
				Debug_Info(PSTR("DFU programming initiated, system going down\n"));
				
				/* Move to programming mode */
				Mode = MODE_DFU;
			}
			break;
		}
	}
}

/***************************************************************************************/
uint8_t Mode_Get(void)
{
	return Mode;
}

