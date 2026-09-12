#ifndef __CST9217_H__
#define __CST9217_H__

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "hardware/i2c.h"
#include "pico/stdlib.h"

// CST9217 I2C address
#define CST9217_I2C_ADDR       0x5A
#define CST9217_CHIP_ID        0x9217

/* CST9217 registers */
#define CST9217_DATA_REG       0xD000
#define CST9217_PROJECT_ID_REG 0xD204
#define CST9217_CMD_MODE_REG   0xD101
#define CST9217_CHECKCODE_REG  0xD1FC
#define CST9217_RESOLUTION_REG 0xD1F8

/* CST9217 parameters */
#define CST9217_ACK_VALUE 0xAB
#define CST9217_MAX_TOUCH_POINTS 1
#define CST9217_DATA_LENGTH (CST9217_MAX_TOUCH_POINTS * 5 + 5)

// Touch point structure
typedef struct {
    uint16_t x;
    uint16_t y;
    bool valid;
} CST9217_Point;

// Main touch structure
typedef struct {
    CST9217_Point data[CST9217_MAX_TOUCH_POINTS];
    uint8_t points;
} CST9217_Struct;

extern CST9217_Struct CST9217;

// Function declarations
void CST9217_Reset(void);
void CST9217_Init(void);
bool CST9217_Read_Config(void);
bool CST9217_Read_Data(void);
#endif