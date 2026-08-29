#include "toyotune_avr.h"
//#include "version.h"
#include "diag.h"
#include "esp.h"

//FUSES = { .low = 0xD4, .high = 0x99, .extended = 0xFF };

uint8_t EEMEM NvSramTestPassed = 0;
uint8_t SramTestPassed;

ESP_t Esp;
uint8_t EspTimerTickPending;

int main(void)
{	
	/* Pull MCU reset low */
	Hal_PioClear(PIO_X_INIT);
	Hal_PioOutput(PIO_X_INIT);

	/* Initialise PIOs */
	PIO_SetInput(PIO_D_HALT);	
	PIO_SetInput(PIO_DIAG_CLK);
	PIO_SetInput(PIO_DIAG_RXD);
	PIO_SetInput(PIO_DIAG_TXD);

	/* input with pull-down */
	PORTB = 0x00;
	DDRC = 0x00;
	
	/* Initialise external memory interface */
	Hal_MemoryExternalInit();
	
	/* Enable memory interface */
	Hal_MemoryExternalEnable();

	/* Run SRAM self-test if it hasn't passed previously */
	Hal_EepromReadBlock(&NvSramTestPassed, &SramTestPassed, sizeof(SramTestPassed));
	if (!SramTestPassed)
	{
		SramTestPassed = Hal_MemorySramTest();
		Hal_EepromWriteBlock(&NvSramTestPassed, &SramTestPassed, sizeof(SramTestPassed));
	}

	/* Check SRAM test has now passed */
	if (SramTestPassed)
	{
		/* MCU is in reset, so write enable SRAM and copy flash to it */
		Hal_MemorySramWriteEnable();
		Hal_MemorySramCopy();
		Hal_MemorySramWriteDisable();

		/* Disable memory interface */
		Hal_MemoryExternalDisable();
	
		/* Release MCU reset */
		Hal_PioSet(PIO_X_INIT);
	}
	
	Diag_Init();

	/* Initialise ECU serial protocol */
	ESP_Init(&Esp);

	Hal_InterruptEnable();
	
	/* Main loop */
	for (;;)	
	{
		/* Kick watchdog */
		Hal_WatchdogKick();
		
		//Diag_Task();

		//ESP_Task(&Esp);

		/* Check if ESP timer tick has occurred */
		if (EspTimerTickPending)
		{
			EspTimerTickPending = 0;
			//ESP_TimerTick(&Esp);
		}
	}
}



// TargetTimerCount = (InputFrequency / Prescale) * Period_MS / 1000
#define TIMER_OCR_VAL (F_CPU / 8UL / 1UL / 1000UL)

ISR(TIMER3_COMPA_vect)
{
	/* Adjust output compare register for next 1ms tick */
	OCR3A += TIMER_OCR_VAL;

	EspTimerTickPending = 1;

	/* Count milliseconds */
	//Diag_TimeMs++;
}

void CALLBACK_ESP_LinkActive(ESP_t *Esp)
{
}

