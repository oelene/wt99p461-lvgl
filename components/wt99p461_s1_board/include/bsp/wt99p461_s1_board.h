/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WT9932P4C61-TINY 精简 BSP 头（学习用）
 * 只暴露：I2C、显示、触摸、背光、RGB LED。
 */

/**
 * @file
 * @brief 精简 BSP: ESP32-P4 WT9932P4C61-TINY
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "bsp/config.h"
#include "bsp/display.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "lvgl.h"
#include "esp_lvgl_port.h"
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

/**************************************************************************************************
 *  BSP Capabilities
 **************************************************************************************************/
#define BSP_CAPS_DISPLAY       1
#define BSP_CAPS_TOUCH         1
#define BSP_CAPS_AUDIO         0
#define BSP_CAPS_SDCARD        0

/**************************************************************************************************
 *  I2C
 **************************************************************************************************/
#if CONFIG_BSP_TOUCH_15PIN_DSI
#define BSP_I2C_SCL (GPIO_NUM_8)
#define BSP_I2C_SDA (GPIO_NUM_7)
#else
#define BSP_I2C_SCL (GPIO_NUM_47)
#define BSP_I2C_SDA (GPIO_NUM_48)
#endif

/**************************************************************************************************
 *  Display and Touch
 **************************************************************************************************/
#define BSP_LCD_BACKLIGHT (GPIO_NUM_20)
#define BSP_LCD_RST       (GPIO_NUM_NC)
#if CONFIG_BSP_TOUCH_15PIN_DSI
#define BSP_LCD_TOUCH_RST (GPIO_NUM_NC)
#define BSP_LCD_TOUCH_INT (GPIO_NUM_NC)
#else
#define BSP_LCD_TOUCH_RST (GPIO_NUM_23)
#define BSP_LCD_TOUCH_INT (GPIO_NUM_21)
#endif

/* 面板方向（UPDN/SHLR 拉高 => 扫描翻转 180°） */
#define BSP_LCD_ENABLE      GPIO_NUM_22
#define BSP_LCD_UPDN        GPIO_NUM_26
#define BSP_LCD_SHLR        GPIO_NUM_27

/* 板载 WS2812（WT9932P4C61-TINY 官方 BSP: GPIO51） */
#define BSP_RGB_LED         GPIO_NUM_51

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************************
 *  I2C interface
 **************************************************************************************************/
#define BSP_I2C_NUM CONFIG_BSP_I2C_NUM

esp_err_t bsp_i2c_init(void);
esp_err_t bsp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/**************************************************************************************************
 *  Display
 **************************************************************************************************/
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

#define BSP_LCD_PIXEL_CLOCK_MHZ (80)
#define BSP_LCD_DRAW_BUFF_SIZE   (BSP_LCD_H_RES * 50)
#define BSP_LCD_DRAW_BUFF_DOUBLE (0)

/**
 * @brief BSP display configuration structure
 */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;
    uint32_t buffer_size;
    bool double_buffer;
    bsp_display_config_t hw_cfg;
    struct {
        unsigned int buff_dma : 1;
        unsigned int buff_spiram : 1;
        unsigned int sw_rotate : 1;
    } flags;
} bsp_display_cfg_t;

lv_display_t *bsp_display_start(void);
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
void bsp_display_stop(lv_display_t *display);
lv_indev_t *bsp_display_get_input_dev(void);

bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation);
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

/**************************************************************************************************
 *  RGB LED (WS2812)
 **************************************************************************************************/
esp_err_t bsp_rgb_led_init(void);
esp_err_t bsp_rgb_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t bsp_rgb_led_clear(void);

#ifdef __cplusplus
}
#endif
