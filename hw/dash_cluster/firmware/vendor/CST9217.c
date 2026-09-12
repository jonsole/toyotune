/******************************************************************************
* | File        :  CST9217.c
* | Author      :  Waveshare Team
* | Function    :  CST9217 Interface Functions
* | Info        :
*----------------
* | This version:  V1.1
* | Date        :  2025-12-10
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
#include "CST9217.h"
#include "DEV_Config.h"

CST9217_Struct CST9217;

/******************************************************************************
function :  Send one byte of data to the specified register of CST9217
parameter:
******************************************************************************/
static void CST9217_I2C_Write_Byte(uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    i2c_write_blocking(I2C_PORT, CST9217_I2C_ADDR, data, 2, false);
}

/******************************************************************************
function :  Send nbytes to the specified register of CST9217
parameter:
******************************************************************************/
static void CST9217_I2C_Write_nByte(uint16_t reg, uint8_t *pData, uint32_t Len) {
    uint8_t data[2 + Len];
    data[0] = reg >> 8;
    data[1] = reg & 0xFF;
    for(uint8_t i = 0; i < Len; i++) {
        data[2 + i] = pData[i];
    }
    i2c_write_blocking(I2C_PORT, CST9217_I2C_ADDR, data, Len, false);
}


/******************************************************************************
function :  Read one byte of data from the specified register of CST9217
parameter:
******************************************************************************/
static uint8_t CST9217_I2C_Read_Byte(uint8_t reg) {
    uint8_t buf;
    i2c_write_blocking(I2C_PORT, CST9217_I2C_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, CST9217_I2C_ADDR, &buf, 1,false);
    return buf;
}

/******************************************************************************
function :  Read nbytes from CST9217
parameter:
******************************************************************************/
static void CST9217_I2C_Read_nByte(uint16_t reg, uint8_t *pData, uint32_t Len) {
    uint8_t data[2];
    data[0] = reg >> 8;
    data[1] = reg & 0xFF;
    i2c_write_blocking(I2C_PORT, CST9217_I2C_ADDR, data, 2, true);
    i2c_read_blocking(I2C_PORT, CST9217_I2C_ADDR, pData, Len, false);
}

/******************************************************************************
function :  Reset the CST9217
parameter:
******************************************************************************/
void CST9217_Reset(void) {
    gpio_put(TOUCH_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(TOUCH_RST_PIN, 1);
    sleep_ms(50);
}

/******************************************************************************
function :  Initialize the CST9217
parameter:
******************************************************************************/
void CST9217_Init(void) {
    // Hardware reset
    CST9217_Reset();

    sleep_ms(30);

    // Read and print configuration
    if(CST9217_Read_Config() == false) {
        printf("Failed to read CST9217 configuration.\n");
        return;
    }
    printf("CST9217 configuration read successfully.\n");
}

bool CST9217_Read_Config(void) {
    uint8_t data[4] = {0};
    uint8_t cmd_mode[2] = {0xD1, 0x01};

    CST9217_I2C_Write_nByte(CST9217_CMD_MODE_REG, cmd_mode, sizeof(cmd_mode));

    sleep_ms(10);
   
    CST9217_I2C_Read_nByte(CST9217_CHECKCODE_REG, data, 4);
       
    printf("Checkcode: 0x%02X%02X%02X%02X\n", data[0], data[1], data[2], data[3]);

    CST9217_I2C_Read_nByte(CST9217_RESOLUTION_REG, data, 4);
    uint16_t res_x = (data[1] << 8) | data[0];
    uint16_t res_y = (data[3] << 8) | data[2];
    printf("Resolution X: %d, Y: %d\n", res_x, res_y);

    CST9217_I2C_Read_nByte(CST9217_PROJECT_ID_REG, data, 4);
    uint16_t chipType = (data[3] << 8) | data[2];
    uint32_t projectID = (data[1] << 8) | data[0];
    printf("Chip Type: 0x%04X, ProjectID: 0x%04lX\n", chipType, projectID);
    return (chipType == CST9217_CHIP_ID);
}

bool CST9217_Read_Data(void) {
    uint8_t data[CST9217_DATA_LENGTH] = {0};

    CST9217_I2C_Read_nByte(CST9217_DATA_REG, data, sizeof(data));

    if (data[6] != CST9217_ACK_VALUE) {
        return false;
    }

    CST9217.points = data[5] & 0x7F;
    CST9217.points = (CST9217.points > CST9217_MAX_TOUCH_POINTS) ? CST9217_MAX_TOUCH_POINTS : CST9217.points;

    for (int i = 0; i < CST9217.points; i++) {
        uint8_t *p = &data[i * 5 + (i ? 2 : 0)];
        uint8_t status = p[0] & 0x0F;

        if (status == 0x06) {
            CST9217.data[i].x = ((p[1] << 4) | (p[3] >> 4));
            CST9217.data[i].y = ((p[2] << 4) | (p[3] & 0x0F));

            CST9217.data[i].x = 466 - CST9217.data[i].x;
            CST9217.data[i].y = 466 - CST9217.data[i].y;

            // printf("Point %d: X=%d, Y=%d\n", i, CST9217.data[i].x, CST9217.data[i].y);
        }
    }
    return true;
}