/*****************************************************************************
* | File      	:   AMOLED_1in75.c
* | Author      :   Waveshare Team
* | Function    :   AMOLED Interface Functions
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2025-12-10
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of theex Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#include "DEV_Config.h"
#include "AMOLED_1in75.h"

AMOLED_1IN75_ATTRIBUTES AMOLED_1IN75;

/********************************************************************************
function:	Sets the start position and size of the display area
parameter:
        qspi    ：  qspi structure
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
		Yend    :   Y direction end coordinates
********************************************************************************/
void AMOLED_1IN75_SetWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend){
    Xstart=Xstart+6;
	Xend=Xend+6;

    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2a); 
    QSPI_DATA_Write(qspi, Xstart>>8);
    QSPI_DATA_Write(qspi, Xstart&0xff);
    QSPI_DATA_Write(qspi, (Xend-1)>>8);
    QSPI_DATA_Write(qspi, (Xend-1)&0xff);
    QSPI_Deselect(qspi); 
    
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2b);
    QSPI_DATA_Write(qspi, Ystart>>8);
    QSPI_DATA_Write(qspi, Ystart&0xff);
    QSPI_DATA_Write(qspi, (Yend-1)>>8);
    QSPI_DATA_Write(qspi, (Yend-1)&0xff);
    QSPI_Deselect(qspi); 
    
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2c);
    QSPI_Deselect(qspi); 
}

/******************************************************************************
function :	Initialize the lcd register
parameter:
        qspi    ：  qspi structure
******************************************************************************/
static void AMOLED_1IN75_InitReg(){
    // reg=0x11, data_bytes=0
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x11);
    sleep_ms(120);
    QSPI_Deselect(qspi);

    // reg=0xc4, data={0x80}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0xc4);
    QSPI_DATA_Write(qspi, 0x80);
    QSPI_Deselect(qspi);

    // reg=0x44, data={0x01, 0xD7}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x44);
    QSPI_DATA_Write(qspi, 0x01);
    QSPI_DATA_Write(qspi, 0xD7);
    QSPI_Deselect(qspi);

    // reg=0x35, data={0x00}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x35);
    QSPI_DATA_Write(qspi, 0x00);
    QSPI_Deselect(qspi);

    // reg=0x53, data={0x20}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x53);
    QSPI_DATA_Write(qspi, 0x20);
    sleep_ms(10);
    QSPI_Deselect(qspi);

    // reg=0x29, data_bytes=0
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x29);
    sleep_ms(10);
    QSPI_Deselect(qspi);

    // reg=0x51, data={0xA0}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x51);
    QSPI_DATA_Write(qspi, 0xA0);
    QSPI_Deselect(qspi);

    // reg=0x20, data_bytes=0  
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x20);
    QSPI_Deselect(qspi);

    // reg=0x36, data={0x00}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x36);
    QSPI_DATA_Write(qspi, 0x00);
    QSPI_Deselect(qspi);

    // reg=0x3A, data={0x05}
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x3A);
    QSPI_DATA_Write(qspi, 0x05);
    QSPI_Deselect(qspi);
}

/********************************************************************************
function :	Reset the lcd
parameter:
        qspi    ：  qspi structure
********************************************************************************/
static void AMOLED_1IN75_Reset(pio_qspi_t qspi){
    gpio_put(qspi.pin_rst,1);
    DEV_Delay_ms(50);
    gpio_put(qspi.pin_rst,0);
    DEV_Delay_ms(50);
    gpio_put(qspi.pin_rst,1);
    DEV_Delay_ms(300);
}

/********************************************************************************
function :	Initialize the lcd
parameter:
        qspi    ：  qspi structure
********************************************************************************/
void AMOLED_1IN75_Init()
{
    //Hardware reset
    AMOLED_1IN75_Reset(qspi);
    
    //Set the initialization register
    AMOLED_1IN75_InitReg();

    AMOLED_1IN75.HEIGHT	= AMOLED_1IN75_HEIGHT;
    AMOLED_1IN75.WIDTH   = AMOLED_1IN75_WIDTH;
}

/******************************************************************************
function :	Set AMOLED Brightness
parameter:
******************************************************************************/
void AMOLED_1IN75_SetBrightness(uint8_t brightness){
    if(brightness > 100) brightness = 100;
    brightness = brightness * 255 / 100;

    // QSPI_1Wrie_Mode(&qspi);
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x51);
    QSPI_DATA_Write(qspi, brightness);
    QSPI_Deselect(qspi);
}


/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void AMOLED_1IN75_Clear(uint16_t Color) {
    // Color data
    uint16_t i;
	uint16_t image[AMOLED_1IN75.HEIGHT];
	for(i=0;i<AMOLED_1IN75.HEIGHT;i++){
		image[i] = Color>>8 | (Color&0xff)<<8;
	}
	uint8_t *partial_image = (uint8_t *)(image);

    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    AMOLED_1IN75_SetWindows(0,0,AMOLED_1IN75.WIDTH,AMOLED_1IN75.HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi,0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    for (int i = 0; i < AMOLED_1IN75.HEIGHT; i++) {
        dma_channel_configure(dma_tx, 
                            &c,
                            &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                            partial_image,            // Source pointer (data buffer)
                            AMOLED_1IN75.WIDTH*2,      // Data length (unit: number of transmissions)
                            true);                    // Start transferring immediately
        
        // Waiting for DMA transfer to complete
        while(dma_channel_is_busy(dma_tx));
    }

    QSPI_Deselect(qspi);
}


/******************************************************************************
function :	Send data to AMOLED to complete full screen refresh
parameter:
        Image   ：  Image data
******************************************************************************/
void AMOLED_1IN75_Display(uint16_t *Image)
{
    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    AMOLED_1IN75_SetWindows(0,0,AMOLED_1IN75.WIDTH,AMOLED_1IN75.HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi,0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    dma_channel_configure(dma_tx, 
                        &c,
                        &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                        (uint8_t *)Image,           // Source pointer (data buffer)
                        AMOLED_1IN75.WIDTH*AMOLED_1IN75.HEIGHT*2,   // Data length (unit: number of transmissions)
                        true);                    // Start transferring immediately
    
    // Waiting for DMA transfer to complete
    while(dma_channel_is_busy(dma_tx));
    QSPI_Deselect(qspi);             
}

/******************************************************************************
function :	Send data to AMOLED to complete partial refresh
parameter:
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
		Yend    :   Y direction end coordinates
        Image   ：  Image data
******************************************************************************/
void AMOLED_1IN75_DisplayWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend, uint16_t *Image) {
    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    AMOLED_1IN75_SetWindows(Xstart, Ystart, Xend, Yend);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

    int i;
    uint32_t pixel_offset;
    uint8_t *partial_image;
    for (i = Ystart; i < Yend; i++) {
        pixel_offset = (i * AMOLED_1IN75.WIDTH + Xstart) * 2;
        partial_image = (uint8_t *)Image + pixel_offset;
        dma_channel_configure(dma_tx, 
                            &c,
                            &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                            partial_image,            // Source pointer (data buffer)
                            (Xend-Xstart)*2,          // Data length (unit: number of transmissions)
                            true);                    // Start transferring immediately

        // Waiting for DMA transfer to complete
        while(dma_channel_is_busy(dma_tx));
    }

    QSPI_Deselect(qspi);
}
