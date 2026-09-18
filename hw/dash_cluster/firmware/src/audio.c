/*
 * audio.c - the ES8311 and the stream that feeds it. See audio.h.
 */

#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "DEV_Config.h"		/* I2C_PORT, shared with touch, the IMU and the RTC */
#include "audio_i2s.pio.h"

/* --- the board ----------------------------------------------------------
 *
 * Pins and clocks are Waveshare's for this module, taken from their own
 * ES8311 example rather than from a schematic - the wiki and the product page
 * publish neither the I2S pin assignment nor the codec's address.
 *
 * GPIO0 is the SPEAKER AMPLIFIER's enable, PA_CTRL in their DEV_Config.h, and
 * nothing is heard without it: the codec only drives a line-level output, and
 * with the amplifier off it plays perfectly into nothing. That is exactly how
 * the first build behaved - chip ID right, stream running, not a sound - so if
 * this ever goes silent again with everything else looking healthy, check this
 * pin first.
 */
#define AUDIO_I2C_ADDR		(0x18u)		/* the ES8311's fixed address */
#define AUDIO_PIN_PA_EN		(0u)		/* speaker amplifier enable, high = on */
#define AUDIO_PIN_DOUT		(1u)		/* MCU -> codec, the DAC's input */
#define AUDIO_PIN_DIN		(2u)		/* codec -> MCU; no microphone here */
#define AUDIO_PIN_MCLK		(3u)		/* MCU -> codec */
#define AUDIO_PIN_BCLK		(4u)		/* codec -> MCU */
#define AUDIO_PIN_LRCK		(5u)		/* codec -> MCU */

/* The wait instructions carry the clock pin numbers inside the PIO program,
   so the two copies must agree. */
#if AUDIO_PIN_BCLK != AUDIO_BCLK_GPIO || AUDIO_PIN_LRCK != AUDIO_LRCK_GPIO
#error "audio.c and audio_i2s.pio disagree about the codec clock pins"
#endif

/* 256 master clocks per sample, which is the ratio the codec coefficient
   table is written around. */
#define AUDIO_MCLK_HZ		(TONE_SAMPLE_RATE * 256u)

/* PIO2. The panel QSPI has PIO0 and can2040 has PIO1 - see PLAN.md 4.1. */
#define AUDIO_PIO		pio2

/* Each buffer is about 21 ms, so the refill interrupt has a whole buffer of
   slack: core 1's worst measured frame is under half of one. Bigger than it
   needs to be on purpose - the cost is 4 KB of SRAM and the failure it buys
   off is an audible stutter. */
#define AUDIO_BUFFER_SAMPLES	(512u)
#define AUDIO_BUFFER_MS		((AUDIO_BUFFER_SAMPLES * 1000u) / TONE_SAMPLE_RATE)

#define AUDIO_I2C_TIMEOUT_US	(5000u)


static bool		Present;
static uint16_t		ChipId;
static uint32_t		Volume;

static uint		SmMclk, SmI2s;
static uint		Chan[2];
static int32_t		Buffer[2][AUDIO_BUFFER_SAMPLES];

/* Written by the loop on core 1, read by the refill interrupt on the same
   core - which is why a single volatile is enough and there is no lock. The
   pattern state itself belongs to the interrupt and is touched nowhere else. */
static volatile uint8_t	Requested;
static Tone_t		Tone;

static volatile uint32_t Buffers;
static volatile uint32_t Underruns;


/***************************************************************************************/
static bool Audio_Write(uint8_t Reg, uint8_t Value)
{
	uint8_t Buf[2] = { Reg, Value };

	return i2c_write_timeout_us(I2C_PORT, AUDIO_I2C_ADDR, Buf, sizeof(Buf), false,
	                            AUDIO_I2C_TIMEOUT_US) == (int)sizeof(Buf);
}


/***************************************************************************************/
static bool Audio_Read(uint8_t Reg, uint8_t *Value)
{
	if (i2c_write_timeout_us(I2C_PORT, AUDIO_I2C_ADDR, &Reg, 1, true,
	                         AUDIO_I2C_TIMEOUT_US) != 1)
		return false;
	return i2c_read_timeout_us(I2C_PORT, AUDIO_I2C_ADDR, Value, 1, false,
	                           AUDIO_I2C_TIMEOUT_US) == 1;
}


/***************************************************************************************/
/* Read, mask, or in, write back. Several of the codec clock registers share a
   byte with something that must be left alone. */
static bool Audio_Modify(uint8_t Reg, uint8_t Keep, uint8_t Set)
{
	uint8_t V;

	if (!Audio_Read(Reg, &V))
		return false;
	V = (uint8_t)((V & Keep) | Set);
	return Audio_Write(Reg, V);
}


/***************************************************************************************/
/*
 * The register sequence, flattened.
 *
 * Waveshare's driver carries a 100-row table of clock coefficients and
 * searches it at runtime for the master clock and sample rate in use. There
 * is exactly one combination here and it will not change, so its row is
 * written out instead:
 *
 *   6.144 MHz MCLK, 24 kHz sample rate: pre-divide 1, pre-multiply 1,
 *   ADC and DAC divide 1, single speed, LRCK divider 0x00FF, BCLK divide 8,
 *   oversample 0x10 both ways.
 *
 * The codec is the serial-port MASTER (register 0x00 bit 6): it takes MCLK
 * and makes its own bit and word clocks, which is what lets the PIO side be a
 * pure slave. Sixteen bits per channel (0x0C in 0x09 and 0x0A) to match the
 * program's sixteen outs.
 *
 * The microphone is deliberately left out. Waveshare power up the analogue
 * PGA and set the ADC gain to maximum; nothing here records anything, and an
 * ADC running into nothing is current and noise for no purpose.
 */
static bool Audio_CodecInit(void)
{
	bool Ok = true;

	/* Reset, then power on. */
	Ok = Ok && Audio_Write(0x00u, 0x1Fu);
	sleep_ms(20);
	Ok = Ok && Audio_Write(0x00u, 0x00u);
	Ok = Ok && Audio_Write(0x00u, 0x80u);

	/* Clocks: everything enabled, MCLK from the pin (bit 7 clear), not
	   inverted (bit 6 clear). */
	Ok = Ok && Audio_Write(0x01u, 0x3Fu);

	/* The coefficient row. 0x02, 0x06 and 0x07 keep bits that are not ours. */
	Ok = Ok && Audio_Modify(0x02u, 0x07u, 0x00u);	/* pre-divide 1, multiply 1 */
	Ok = Ok && Audio_Write(0x03u, 0x10u);		/* single speed, ADC osr 0x10 */
	Ok = Ok && Audio_Write(0x04u, 0x10u);		/* DAC osr */
	Ok = Ok && Audio_Write(0x05u, 0x00u);		/* ADC and DAC divide 1 */
	Ok = Ok && Audio_Modify(0x06u, 0xE0u, 0x07u);	/* BCLK divide 8, not inverted */
	Ok = Ok && Audio_Modify(0x07u, 0xC0u, 0x00u);	/* LRCK divider, high bits */
	Ok = Ok && Audio_Write(0x08u, 0xFFu);		/* and low */

	/* Serial port: codec is master, I2S, 16 bits each way. */
	Ok = Ok && Audio_Modify(0x00u, 0xFFu, 0x40u);
	Ok = Ok && Audio_Write(0x09u, 0x0Cu);
	Ok = Ok && Audio_Write(0x0Au, 0x0Cu);

	/* Analogue on, DAC on, output to the driver. */
	Ok = Ok && Audio_Write(0x0Du, 0x01u);
	Ok = Ok && Audio_Write(0x12u, 0x00u);
	Ok = Ok && Audio_Write(0x13u, 0x10u);
	Ok = Ok && Audio_Write(0x37u, 0x08u);		/* bypass the DAC equaliser */

	return Ok;
}


/***************************************************************************************/
void Audio_SetVolume(uint32_t Percent)
{
	uint8_t Reg;

	if (Percent > 100u)
		Percent = 100u;
	Volume = Percent;

	if (!Present)
		return;

	/* Waveshare's mapping, so a number means the same here as in their example.
	   Note it is NOT perceptual: the register is 0.5 dB a step with 0xBF as
	   0 dB, so 73 is about -2.5 dB, 55 about -26 dB and 30 about -57 dB -
	   anything much below 50 is close to silent. */
	Reg = (Percent == 0u) ? 0u : (uint8_t)(((Percent * 256u) / 100u) - 1u);
	(void)Audio_Write(0x32u, Reg);
}


/***************************************************************************************/
uint32_t Audio_Volume(void)
{
	return Volume;
}


/***************************************************************************************/
/*
 * Refill whichever buffer has just drained.
 *
 * The two channels chain to one another, so the stream is continuous and the
 * only job here is to put the finished channel back where it started: a
 * completed channel's transfer count reads zero, and the chain will use
 * whatever it finds. Both interrupts pending at once means this ran a whole
 * buffer late and the DMA has been replaying stale audio - counted, because
 * it is audible and it means something on core 1 blocked for 21 ms.
 *
 * Lowest interrupt priority, so the panel's tearing-effect interrupt always
 * wins: a late frame is visible and a late beep is not.
 */
static void Audio_Irq(void)
{
	uint32_t Both = (1u << Chan[0]) | (1u << Chan[1]);
	uint32_t Pending = dma_hw->ints1 & Both;
	uint32_t i;

	if (Pending == Both)
		Underruns++;

	for (i = 0; i < 2u; i++)
	{
		if ((Pending & (1u << Chan[i])) == 0u)
			continue;

		dma_hw->ints1 = 1u << Chan[i];

		Tone_Set(&Tone, (ToneId_t)Requested);
		Tone_Fill(&Tone, Buffer[i], AUDIO_BUFFER_SAMPLES);

		dma_channel_set_read_addr(Chan[i], Buffer[i], false);
		dma_channel_set_trans_count(Chan[i], AUDIO_BUFFER_SAMPLES, false);
		Buffers++;
	}
}


/***************************************************************************************/
static void Audio_StreamInit(void)
{
	uint OffsetMclk, OffsetI2s;
	uint32_t i;

	Tone_Init();
	Tone_Reset(&Tone);
	Requested = (uint8_t)TONE_NONE;
	memset(Buffer, 0, sizeof(Buffer));

	/* The master clock first and on its own: the codec cannot produce the bit
	   and word clocks the data machine waits on until it has one. */
	SmMclk = (uint)pio_claim_unused_sm(AUDIO_PIO, true);
	OffsetMclk = (uint)pio_add_program(AUDIO_PIO, &audio_mclk_program);
	audio_mclk_program_init(AUDIO_PIO, SmMclk, OffsetMclk, AUDIO_PIN_MCLK,
	                        AUDIO_MCLK_HZ);
	pio_sm_set_enabled(AUDIO_PIO, SmMclk, true);

	SmI2s = (uint)pio_claim_unused_sm(AUDIO_PIO, true);
	OffsetI2s = (uint)pio_add_program(AUDIO_PIO, &audio_i2s_program);
	audio_i2s_program_init(AUDIO_PIO, SmI2s, OffsetI2s, AUDIO_PIN_DOUT,
	                       AUDIO_PIN_BCLK, AUDIO_PIN_LRCK);

	/* Both claimed before either is configured: each is chained to the other,
	   so the second channel's number has to exist before the first can name
	   it. */
	for (i = 0; i < 2u; i++)
		Chan[i] = (uint)dma_claim_unused_channel(true);

	for (i = 0; i < 2u; i++)
	{
		dma_channel_config C = dma_channel_get_default_config(Chan[i]);

		channel_config_set_transfer_data_size(&C, DMA_SIZE_32);
		channel_config_set_read_increment(&C, true);
		channel_config_set_write_increment(&C, false);
		channel_config_set_dreq(&C, pio_get_dreq(AUDIO_PIO, SmI2s, true));
		channel_config_set_chain_to(&C, Chan[1u - i]);
		dma_channel_configure(Chan[i], &C, &AUDIO_PIO->txf[SmI2s], Buffer[i],
		                      AUDIO_BUFFER_SAMPLES, false);
		dma_channel_set_irq1_enabled(Chan[i], true);
	}

	irq_set_exclusive_handler(DMA_IRQ_1, Audio_Irq);
	irq_set_priority(DMA_IRQ_1, PICO_LOWEST_IRQ_PRIORITY);
	irq_set_enabled(DMA_IRQ_1, true);

	pio_sm_set_enabled(AUDIO_PIO, SmI2s, true);
	dma_channel_start(Chan[0]);
}


/***************************************************************************************/
bool Audio_Init(void)
{
	uint8_t Id1 = 0, Id2 = 0;

	Volume = AUDIO_VOLUME_PCT;

	/* Who is there. A NAK here is the normal answer on a board with no codec
	   fitted, and is not a failure of anything else. */
	if (!Audio_Read(0xFDu, &Id1) || !Audio_Read(0xFEu, &Id2))
	{
		printf("audio: no codec at 0x%02x\n", (unsigned)AUDIO_I2C_ADDR);
		return false;
	}
	ChipId = (uint16_t)(((uint16_t)Id1 << 8) | Id2);

	if (!Audio_CodecInit())
	{
		printf("audio: codec 0x%04x would not configure\n", (unsigned)ChipId);
		return false;
	}

	Present = true;
	Audio_SetVolume(Volume);
	Audio_StreamInit();

	/* The amplifier last, once the codec has a clock and the stream is
	   playing zeros: switching it on into a codec still coming out of reset is
	   the likeliest way to get a thump at power-up. */
	gpio_init(AUDIO_PIN_PA_EN);
	gpio_set_dir(AUDIO_PIN_PA_EN, GPIO_OUT);
	gpio_put(AUDIO_PIN_PA_EN, 1);

	printf("audio: ES8311 0x%04x, %lu Hz, %lu%% volume, %lu ms buffers\n",
	       (unsigned)ChipId, (unsigned long)TONE_SAMPLE_RATE,
	       (unsigned long)Volume, (unsigned long)AUDIO_BUFFER_MS);
	return true;
}


/***************************************************************************************/
bool Audio_Present(void)
{
	return Present;
}


/***************************************************************************************/
void Audio_Warn(ToneId_t Id)
{
	if ((uint32_t)Id >= (uint32_t)TONE_COUNT)
		Id = TONE_NONE;
	Requested = (uint8_t)Id;
}


/***************************************************************************************/
ToneId_t Audio_Sounding(void)
{
	return (ToneId_t)Requested;
}


/***************************************************************************************/
uint32_t Audio_Buffers(void)
{
	return Buffers;
}


/***************************************************************************************/
uint32_t Audio_Underruns(void)
{
	return Underruns;
}


/***************************************************************************************/
uint16_t Audio_ChipId(void)
{
	return ChipId;
}
