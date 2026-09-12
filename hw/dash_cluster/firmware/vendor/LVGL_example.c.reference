/*****************************************************************************
* | File      	:   LCD_1IN75_LVGL_test.c
* | Author      :   Waveshare team
* | Function    :   2.41inch LCD  test demo
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2024-06-27
* | Info        :
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
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
#
******************************************************************************/
#include "LVGL_example.h" 
#include "src/core/lv_obj.h"
#include "src/misc/lv_area.h"
#include "pico/multicore.h"
#include "psram_tool.h"
#include "rp_pico_alloc.h"

// LVGL
static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t *buf0;

static lv_obj_t *tv;
static lv_obj_t *tile1;
static lv_obj_t *tile2;
static lv_obj_t *tile3;
static lv_obj_t *tile4;

static lv_obj_t *table_imu_data;
static lv_obj_t *table_rtc_date;
static lv_obj_t *table_rtc_time;
static lv_obj_t *roller;
static lv_obj_t *lable;

// Touch
static uint16_t ts_x;
static uint16_t ts_y;
static uint8_t gesture = 0;
static lv_indev_state_t ts_act;
static lv_indev_drv_t indev_ts;

// Timer 
static struct repeating_timer lvgl_timer;
static struct repeating_timer rtc_update_timer;
static struct repeating_timer imu_update_timer;
static void disp_flush_cb(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p);
static void touch_callback(uint gpio, uint32_t events);
static void ts_read_cb(lv_indev_drv_t * drv, lv_indev_data_t*data);
static void dma_handler(void);
static void roller_event_cb(lv_event_t * event);
static void update_imu_data(void);
static bool repeating_lvgl_timer_callback(struct repeating_timer *t); 
static bool repeating_touch_callback(struct repeating_timer *t); 
static bool repeating_imu_update_timer_callback(struct repeating_timer *t); 
static bool repeating_rtc_update_timer_callback(struct repeating_timer *t);

/********************************************************************************
function : Initializes LVGL and enbable timers IRQ and DMA IRQ
parameter:
********************************************************************************/
void LVGL_Init(void)
{
    /*1.Init Timer*/ 
    add_repeating_timer_ms(300,  repeating_rtc_update_timer_callback, NULL, &rtc_update_timer);
    add_repeating_timer_ms(500,  repeating_imu_update_timer_callback, NULL, &imu_update_timer);
    add_repeating_timer_ms(5,    repeating_lvgl_timer_callback,       NULL, &lvgl_timer);
    
    /*2.Init LVGL core*/
    lv_init();

    /*3.Init LVGL display*/
    buf0 = (lv_color_t *)malloc(DISP_HOR_RES * DISP_VER_RES);
    lv_disp_draw_buf_init(&disp_buf, buf0, NULL, DISP_HOR_RES * DISP_VER_RES); 
    lv_disp_drv_init(&disp_drv);    
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &disp_buf;        
    disp_drv.hor_res = DISP_HOR_RES;
    disp_drv.ver_res = DISP_VER_RES;
    lv_disp_t *disp= lv_disp_drv_register(&disp_drv);   

#if INPUTDEV_TS
    /*4.Init touch screen as input device*/ 
    lv_indev_drv_init(&indev_ts); 
    indev_ts.type = LV_INDEV_TYPE_POINTER;    
    indev_ts.read_cb = ts_read_cb;            
    lv_indev_t * ts_indev = lv_indev_drv_register(&indev_ts);
    //Enable IRQ
    DEV_KEY_Config(TOUCH_INT_PIN);
    DEV_IRQ_SET(TOUCH_INT_PIN, GPIO_IRQ_EDGE_RISE, &touch_callback);
    DEV_KEY_Config(SYS_OUT);
    DEV_IRQ_SET(SYS_OUT, GPIO_IRQ_EDGE_FALL, &touch_callback);
#endif

    /*5.Init DMA for transmit color data from memory to SPI*/
    dma_channel_set_irq0_enabled(dma_tx, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

/********************************************************************************
function : Initializes the layout of LVGL widgets
parameter:
********************************************************************************/
void Widgets_Init(void)
{
    /*Style Config*/
    static lv_style_t style_roller;
    lv_style_init(&style_roller);
    lv_style_set_border_color(&style_roller, lv_palette_darken(LV_PALETTE_BLUE, 3));
    lv_style_set_text_font(&style_roller, &lv_font_montserrat_26);
    
    static lv_style_t style_imu_table;
    lv_style_init(&style_imu_table);
    lv_style_set_border_width(&style_imu_table, 1);
    lv_style_set_text_font(&style_imu_table, &lv_font_montserrat_26);

    static lv_style_t style_rtc_table;
    lv_style_init(&style_rtc_table);
    lv_style_set_border_width(&style_rtc_table, 1);
    lv_style_set_text_font(&style_rtc_table, &lv_font_montserrat_26);

    static lv_style_t style_lable;
    lv_style_init(&style_lable);
    lv_style_set_text_font(&style_lable, &lv_font_montserrat_26);

    /*Create tileview*/
    tv = lv_tileview_create(lv_scr_act());
    lv_obj_set_scrollbar_mode(tv,  LV_SCROLLBAR_MODE_OFF);

    /*Tile1: Just a pic*/
    tile1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM);
    
    LV_IMG_DECLARE(pic);
    lv_obj_t *img1 = lv_img_create(tile1);
    lv_img_set_src(img1, &pic);
    lv_obj_align(img1, LV_ALIGN_CENTER, 0, 0);
 
    /*Tile2: Show IMU data*/
    tile2 = lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP|LV_DIR_BOTTOM);
    
    lv_obj_t *table = lv_table_create(tile2);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table, 130, 366); 
    lv_obj_align(table, LV_ALIGN_LEFT_MID, 60 ,0); 
    lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table, 0, 0, "ACC_X");
    lv_table_set_cell_value(table, 1, 0, "ACC_Y");
    lv_table_set_cell_value(table, 2, 0, "ACC_Z");
    lv_table_set_cell_value(table, 3, 0, "CYR_X");
    lv_table_set_cell_value(table, 4, 0, "CYR_Y");
    lv_table_set_cell_value(table, 5, 0, "CYR_Z");
    lv_obj_set_style_bg_color(table, lv_palette_main(LV_PALETTE_LIGHT_BLUE), LV_PART_ITEMS);
    lv_obj_add_style(table, &style_imu_table, 0); 

    table_imu_data = lv_table_create(tile2);
    lv_obj_clear_flag(table_imu_data, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table_imu_data, 124, 366); 
    lv_obj_align(table_imu_data, LV_ALIGN_CENTER, 17, 0); 
    lv_obj_clear_flag(table_imu_data, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table_imu_data, 0, 0, "0");
    lv_table_set_cell_value(table_imu_data, 1, 0, "0");
    lv_table_set_cell_value(table_imu_data, 2, 0, "0");
    lv_table_set_cell_value(table_imu_data, 3, 0, "0");
    lv_table_set_cell_value(table_imu_data, 4, 0, "0");
    lv_table_set_cell_value(table_imu_data, 5, 0, "0");
    lv_obj_set_style_bg_color(table_imu_data, lv_color_make(250, 144, 181), LV_PART_ITEMS);
    lv_obj_add_style(table_imu_data, &style_imu_table, 0); 

    table = lv_table_create(tile2);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table, 100, 366); 
    lv_obj_align(table, LV_ALIGN_RIGHT_MID, -60, 0); 
    lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table, 0, 0, "mg");
    lv_table_set_cell_value(table, 1, 0, "mg");
    lv_table_set_cell_value(table, 2, 0, "mg");
    lv_table_set_cell_value(table, 3, 0, "dps"); 
    lv_table_set_cell_value(table, 4, 0, "dps");
    lv_table_set_cell_value(table, 5, 0, "dps");
    lv_obj_set_style_bg_color(table, lv_palette_main(LV_PALETTE_LIGHT_BLUE), LV_PART_ITEMS);
    lv_obj_add_style(table, &style_imu_table, 0); 

    /*Tile3: Show RTC data*/
    tile3 = lv_tileview_add_tile(tv, 0, 2, LV_DIR_TOP|LV_DIR_BOTTOM);

    table = lv_table_create(tile3);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table, 300, 60);
    lv_table_set_col_width(table, 0, 100);
    lv_table_set_col_width(table, 1, 100);
    lv_table_set_col_width(table, 2, 100);
    lv_obj_align(table, LV_ALIGN_CENTER, 1, -88);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table, 0, 0, "Year");
    lv_table_set_cell_value(table, 0, 1, "Mon");
    lv_table_set_cell_value(table, 0, 2, "Day");
    lv_obj_set_style_bg_color(table, lv_palette_main(LV_PALETTE_LIGHT_BLUE), LV_PART_ITEMS);
    lv_obj_add_style(table, &style_rtc_table, 0);

    table_rtc_date = lv_table_create(tile3);
    lv_obj_clear_flag(table_rtc_date, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table_rtc_date, 300, 60);
    lv_table_set_col_width(table_rtc_date, 0, 100);
    lv_table_set_col_width(table_rtc_date, 1, 100);
    lv_table_set_col_width(table_rtc_date, 2, 100);
    lv_obj_align(table_rtc_date, LV_ALIGN_CENTER, 1, -30);
    lv_obj_clear_flag(table_rtc_date, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table_rtc_date, 0, 0, "0");
    lv_table_set_cell_value(table_rtc_date, 0, 1, "0");
    lv_table_set_cell_value(table_rtc_date, 0, 2, "0");
    lv_obj_set_style_bg_color(table_rtc_date, lv_color_make(250, 144, 181), LV_PART_ITEMS);
    lv_obj_add_style(table_rtc_date, &style_rtc_table, 0);

    table = lv_table_create(tile3);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table, 300, 60);
    lv_table_set_col_width(table, 0, 100);
    lv_table_set_col_width(table, 1, 100);
    lv_table_set_col_width(table, 2, 100);
    lv_obj_align(table, LV_ALIGN_CENTER, 1, 30);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table, 0, 0, "Hour");
    lv_table_set_cell_value(table, 0, 1, "Min");
    lv_table_set_cell_value(table, 0, 2, "Sec");
    lv_obj_set_style_bg_color(table, lv_palette_main(LV_PALETTE_LIGHT_BLUE), LV_PART_ITEMS);
    lv_obj_add_style(table, &style_rtc_table, 0);

    table_rtc_time = lv_table_create(tile3);
    lv_obj_clear_flag(table_rtc_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(table_rtc_time, 300, 60);
    lv_table_set_col_width(table_rtc_time, 0, 100);
    lv_table_set_col_width(table_rtc_time, 1, 100);
    lv_table_set_col_width(table_rtc_time, 2, 100);
    lv_obj_align(table_rtc_time, LV_ALIGN_CENTER, 1, 88);
    lv_obj_clear_flag(table_rtc_time, LV_OBJ_FLAG_SCROLLABLE);
    lv_table_set_cell_value(table_rtc_time, 0, 0, "0");
    lv_table_set_cell_value(table_rtc_time, 0, 1, "0");
    lv_table_set_cell_value(table_rtc_time, 0, 2, "0");
    lv_obj_set_style_bg_color(table_rtc_time, lv_color_make(250, 144, 181), LV_PART_ITEMS);
    lv_obj_add_style(table_rtc_time, &style_rtc_table, 0);

    /*tile4: AMOLED Brightness*/
    tile4 = lv_tileview_add_tile(tv, 0, 3, LV_DIR_TOP);

    lv_obj_t *lable = lv_label_create(tile4);
    lv_label_set_text(lable, "AMOLED Brightness");
    lv_obj_align(lable, LV_ALIGN_CENTER, 0, -110);
    lv_obj_add_style(lable, &style_lable, 0); 

    roller = lv_roller_create(tile4);
    const char * opts = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10";
    lv_roller_set_options(roller, opts, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_pos(roller, 184, 200);
    lv_obj_set_height(roller, 180);
    lv_obj_set_width(roller, 80);
    lv_roller_set_selected(roller, 5, LV_ANIM_OFF);
    lv_obj_add_style(roller, &style_roller,0);
    lv_obj_add_event_cb(roller, roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /*Initialize Display*/
    update_imu_data(); 
}

/********************************************************************************
function : Refresh image by transferring the color data to the SPI bus by DMA
parameter:
********************************************************************************/
static void disp_flush_cb(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p)
{
    AMOLED_1IN75_SetWindows(area->x1, area->y1, area->x2+1 , area->y2+1);  // Se.t the LVGL interface display position
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);

    dma_channel_configure(dma_tx,   
                          &c,
                          &qspi.pio->txf[qspi.sm], 
                          color_p, // read address
                          ((area->x2 + 1 - area->x1) * (area->y2 + 1 - area->y1))*2,
                          true);// Start DMA transfer
}

/********************************************************************************
function : Touch interrupt handler
parameter:
********************************************************************************/
static void touch_callback(uint gpio, uint32_t events)
{
    if (gpio == TOUCH_INT_PIN)
    {
        CST9217_Read_Data(); // Get coordinate data
        ts_x = CST9217.data[0].x;
        ts_y = CST9217.data[0].y;
        ts_act = LV_INDEV_STATE_PRESSED;
    }
    else if(gpio == SYS_OUT)
    {
        watchdog_reboot(0,0,0);
    }
}

/********************************************************************************
function : Update touch screen input device status
parameter:
********************************************************************************/
static void ts_read_cb(lv_indev_drv_t * drv, lv_indev_data_t*data)
{
    data->point.x = ts_x;
    data->point.y = ts_y; 
    data->state = ts_act;
    ts_act = LV_INDEV_STATE_RELEASED;
}

/********************************************************************************
function : Indicate ready with the flushing when DMA complete transmission
parameter:
********************************************************************************/
static void dma_handler(void)
{
    if (dma_channel_get_irq0_status(dma_tx)) 
    {
        dma_channel_acknowledge_irq0(dma_tx);
        QSPI_Deselect(qspi);   
        lv_disp_flush_ready(&disp_drv); // Indicate you are ready with the flushing
    }
}

/********************************************************************************
function : Check if the page needs to be updated
parameter:
********************************************************************************/
static bool update_check(lv_obj_t *tv,lv_obj_t *tilex) 
{
    uint8_t ret = true; 

    lv_obj_t *active_tile = lv_tileview_get_tile_act(tv); // Get the current active interface
    if (active_tile != tilex) 
    {
        ret = false;
    }

    return ret;
}

/********************************************************************************
function : Update IMU data
parameter:
********************************************************************************/
static void update_imu_data()
{
    float acc[3], gyro[3];
    unsigned int tim_count = 0;
    QMI8658_read_xyz(acc, gyro, &tim_count); // Reading IMU data

    char table_text[64];
    for(int i = 0; i < 3; i++)
    {
        sprintf(table_text,"%4.1f",acc[i]);
        lv_table_set_cell_value(table_imu_data, i, 0, table_text); // Update table data
    }

    for(int i = 0; i < 3; i++)
    {
        sprintf(table_text,"%4.1f",gyro[i]);
        lv_table_set_cell_value(table_imu_data, i+3, 0, table_text);
    }
}

/********************************************************************************
function : Update RTC data
parameter:
********************************************************************************/
static void update_rtc_data()
{
    datetime_t Now_time;
    PCF85063A_Read_now(&Now_time); //Reading RTC dat1a

    char table_text[64];
    sprintf(table_text,"%d",Now_time.year);
    lv_table_set_cell_value(table_rtc_date, 0, 0, table_text); // Update table data
    sprintf(table_text,"%02d",Now_time.month);
    lv_table_set_cell_value(table_rtc_date, 0, 1, table_text);
    sprintf(table_text,"%02d",Now_time.day);
    lv_table_set_cell_value(table_rtc_date, 0, 2, table_text);
    sprintf(table_text,"%02d",Now_time.hour);
    lv_table_set_cell_value(table_rtc_time, 0, 0, table_text);
    sprintf(table_text,"%02d",Now_time.min);
    lv_table_set_cell_value(table_rtc_time, 0, 1, table_text);
    sprintf(table_text,"%02d",Now_time.sec);
    lv_table_set_cell_value(table_rtc_time, 0, 2, table_text);
}

/********************************************************************************
function : Report the elapsed time to LVGL each 5ms
parameter:
********************************************************************************/
static bool repeating_lvgl_timer_callback(struct repeating_timer *t) 
{
    lv_tick_inc(5);
    return true;
}

/********************************************************************************
function : Update IMU label data each 500ms
parameter:
********************************************************************************/
static bool repeating_imu_update_timer_callback(struct repeating_timer *t) 
{
    if(update_check(tv,tile2) == true) // Need to update the interface
        update_imu_data(); // Update data

    return true;
}

/********************************************************************************
function : Update RTC label data each 300ms
parameter:
********************************************************************************/
static bool repeating_rtc_update_timer_callback(struct repeating_timer *t)
{
    if(update_check(tv,tile3) == true) // Need to update the interface
        update_rtc_data(); // Update data

    return true;
}

/********************************************************************************
function : Change LCD brightness
parameter:
********************************************************************************/
static void roller_event_cb(lv_event_t * event) 
{
    lv_obj_t * obj = lv_event_get_target(event); // Get the object that triggered the event
    int selected_index = lv_roller_get_selected(obj); // Get the index of the currently selected item of the scroll bar
    AMOLED_1IN75_SetBrightness((selected_index+1)*10); // Set the output level of the PWM channel according to the selected item index
}
