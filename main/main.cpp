/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * WT9932P4C61-TINY —— FreeRTOS + LVGL 学习工程（精简版）
 *
 * 目的：在保留屏幕/触摸基础驱动的前提下，提供一个干净的 FreeRTOS + LVGL 学习入口。
 *
 * 开机流程：
 *   1. 初始化 NVS
 *   2. 初始化显示 + 触摸（BSP 内部会启动 LVGL 任务，屏幕驱动见 components/esp_lcd_jd9165）
 *   3. 点亮背光、初始化板载 RGB LED
 *   4. 创建 LVGL 界面（计数器 + 开关 + LED 联动）
 *   5. 另起一个 FreeRTOS 任务，定期打印内存 + 点灯呼吸
 *
 * 想学 LVGL：看下面 ui_create_ui()。
 * 想学 FreeRTOS：看下面 board_task()、以及 BSP 里 lvgl_port 任务。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

static const char *TAG = "app_main";

/* LVGL 端口任务配置：优先级、栈、刷新周期 */
#define LVGL_PORT_INIT_CONFIG()   \
    {                             \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

/* ============================ LVGL 界面 ============================ */

/* 计数器标签 */
static lv_obj_t *g_count_label = NULL;
static int g_count = 0;
static int R_count = 0;
/* LED 开关（与板载 WS2812 联动） */
static lv_obj_t *g_led_switch = NULL;

static void count_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        g_count++;
        lv_label_set_text_fmt(g_count_label, "Count: %d", g_count);
        ESP_LOGI(TAG, "button clicked, count=%d", g_count);
    }
}


static void count_btn_R_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        R_count++;
        lv_label_set_text_fmt(g_count_label, "RCount: %d", R_count);
        ESP_LOGI(TAG, "button clicked, Rcount=%d", R_count);
    }
}




static void led_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (on) {
        bsp_rgb_led_set_rgb(R_count, 64, 64); // 青色
        ESP_LOGI(TAG, "LED switch ON");
    } else {
        bsp_rgb_led_clear();
        ESP_LOGI(TAG, "LED switch OFF");
    }
}



/* 创建学习界面：标题 + 计数器 + 按钮 + LED 开关 */
static void ui_create_ui(void)
{
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WT9932P4C61-TINY  FreeRTOS+LVGL");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* 副标题 */
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "helloword");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9DB2C0), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 90);

    /* 计数器标签 */
    g_count_label = lv_label_create(scr);
    lv_label_set_text(g_count_label, "Count: 0");
    lv_obj_set_style_text_color(g_count_label, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_text_font(g_count_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_count_label, LV_ALIGN_CENTER, 0, -80);

    /* 计数按钮 */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 260, 96);
    lv_obj_align(btn, LV_ALIGN_CENTER, -160, 40);
    lv_obj_add_event_cb(btn, count_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "+1  Click");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(btn_label);

    lv_obj_t *btn2 = lv_btn_create(scr);
    lv_obj_set_size(btn2, 300, 96);
    lv_obj_align(btn2, LV_ALIGN_CENTER, 160, 40);
    lv_obj_add_event_cb(btn2, count_btn_R_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn2_label = lv_label_create(btn2);
    lv_label_set_text(btn2_label, "R +1");
    lv_obj_set_style_text_font(btn2_label, &lv_font_montserrat_28, 0);
    lv_obj_center(btn2_label);

    /* LED 开关 */
    lv_obj_t *led_label = lv_label_create(scr);
    lv_label_set_text(led_label, "LED");
    lv_obj_set_style_text_color(led_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(led_label, &lv_font_montserrat_20, 0);
    lv_obj_align(led_label, LV_ALIGN_CENTER, -120, 180);

    g_led_switch = lv_switch_create(scr);
    lv_obj_set_size(g_led_switch, 100, 50);
    lv_obj_align(g_led_switch, LV_ALIGN_CENTER, 60, 180);
    lv_obj_add_event_cb(g_led_switch, led_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    bsp_display_unlock();
}

/* ============================ FreeRTOS 任务 ============================ */

/* 后台任务：每 5 秒打印内存，并让 LED 呼吸（学习 FreeRTOS 周期任务 + LVGL 线程安全） */
static void board_task(void *arg)
{
    (void)arg;
    bool led_on = false;
    while (1) {
        size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        ESP_LOGI(TAG, "MEM  SRAM free=%u KB  PSRAM free=%u KB  (tick=%u)",
                 (unsigned)sram_free, (unsigned)psram_free, (unsigned)xTaskGetTickCount());

        /* 若用户没开 LED 开关，就让板载灯缓慢闪烁，证明任务在跑 */
        bsp_display_lock(0);
        bool sw_on = g_led_switch && lv_obj_has_state(g_led_switch, LV_STATE_CHECKED);
        bsp_display_unlock();

        if (!sw_on) {
            led_on = !led_on;
            if (led_on) {
                bsp_rgb_led_set_rgb(16, 0, 24); // 暗紫色
            } else {
                bsp_rgb_led_clear();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ============================ 入口 ============================ */

extern "C" void app_main(void)
{
    printf("%s %s\n", DEV_MODEL, SOFT_VERSION);

    /* 1. NVS（Wi-Fi / LVGL 配置会用到） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2. 显示 + 触摸（BSP 内部启动 LVGL 任务） */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg =
            {
                .hdmi_resolution = BSP_HDMI_RES_NONE,
                .dsi_bus =
                    {
                        .phy_clk_src = static_cast<mipi_dsi_phy_clock_source_t>(0),
                        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                    },
            },
        .flags =
            {
                .buff_dma = false,
                .buff_spiram = true,
                .sw_rotate = false,
            },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "Display init failed; stop");
        return;
    }
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display + LVGL ready (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);

    /* 3. 板载 RGB LED */
    if (bsp_rgb_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed (ignored)");
    }

    /* 4. 建立 LVGL 界面 */
    ui_create_ui();

    /* 5. 后台任务（示例：打印内存 + 闪烁 LED） */
    xTaskCreate(board_task, "board_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "FreeRTOS + LVGL demo started");
}
