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
 *   4. 创建 LVGL 界面（计数器按钮 + RGB 滑动条 + LED 开关联动）
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
#include "driver/gpio.h"
#include "driver/ledc.h"
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
/* RGB 滑动条：[0]=R [1]=G [2]=B（同样联动 WS2812） */
static lv_obj_t *g_rgb_sliders[3] = {NULL, NULL, NULL};

/* IO28 翻转按钮：每按一次电平取反，按钮文字显示当前电平 */
#define IO28_GPIO GPIO_NUM_28
static lv_obj_t *g_io28_btn_label = NULL;
static int g_io28_level = 0;

static void io28_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        g_io28_level = !g_io28_level;
        gpio_set_level(IO28_GPIO, g_io28_level);
        lv_label_set_text_fmt(g_io28_btn_label, "IO28: %d", g_io28_level);
        ESP_LOGI(TAG, "IO28 toggled -> %d", g_io28_level);
    }
}

/* ============================ PWM 输出（LEDC） ============================ */

/* P4 没有硬件 DAC，用 LEDC PWM 合成可调模拟电压：
 * 引脚输出 20 kHz 方波，占空比 0~100% 由滑动条控制。
 * 想要平滑直流，板外接 RC 低通即可（如 1kΩ 串联 + 1µF 对地，
 * 截止频率约 160 Hz，滤完的电压 ≈ 3.3V × 占空比）。 */
#define PWM_GPIO        GPIO_NUM_9
#define PWM_FREQ_HZ     20000                       /* 20 kHz：听不见，RC 也好滤 */
#define PWM_RESOLUTION  LEDC_TIMER_10_BIT           /* 10 位分辨率：duty 0~1023 */

static lv_obj_t *g_pwm_slider = NULL;
static lv_obj_t *g_pwm_val_label = NULL;

static void pwm_init(void)
{
    /* P4 的 LEDC 只有低速通道，且所有 timer 共享一个全局时钟源。
     * BSP 背光已经用 LEDC_AUTO_CLK 占了时钟源（timer 1 / channel 1），
     * 这里必须也用 LEDC_AUTO_CLK 保持一致，否则 ledc_timer_config 报
     * "timer clock conflict" 直接 ESP_FAIL。 */
    const ledc_timer_config_t pwm_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PWM_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&pwm_timer));

    const ledc_channel_config_t pwm_ch = {
        .gpio_num = PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0, /* 初始占空比 0%：输出常低 */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&pwm_ch));
    ESP_LOGI(TAG, "PWM ready on GPIO%d: %u Hz, 10-bit duty", PWM_GPIO, PWM_FREQ_HZ);
}

/* 滑动条回调：0~100% 写入 LEDC 占空比，并刷新数值标签（user_data = 数值标签） */
static void pwm_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *val_label = (lv_obj_t *)lv_event_get_user_data(e);

    int32_t pct = lv_slider_get_value(slider);
    uint32_t duty = (uint32_t)pct * 1023 / 100; /* 0~100% → 0~1023 */
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    /* 显示占空比 + RC 滤波后的估算电压。用毫伏整数算，避免依赖浮点 printf */
    uint32_t mv = (uint32_t)pct * 3300 / 100;
    lv_label_set_text_fmt(val_label, "%d%%  %d.%02dV", (int)pct,
                          (int)(mv / 1000), (int)((mv % 1000) / 10));
    ESP_LOGI(TAG, "PWM duty=%d%% (raw=%u)", (int)pct, (unsigned)duty);
}

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
        /* 滑动条的“值”必须用 API 读写，指针本身不能动 */
        int32_t v = lv_slider_get_value(g_rgb_sliders[0]);
        if (v < 255) {
            v++;
        }
        lv_slider_set_value(g_rgb_sliders[0], v, LV_ANIM_OFF);
        /* 主动发事件，复用滑动条自己的回调：刷新右侧数值标签 + LED */
        lv_event_send(g_rgb_sliders[0], LV_EVENT_VALUE_CHANGED, NULL);
        lv_label_set_text_fmt(g_count_label, "R: %d", v);
        ESP_LOGI(TAG, "R button clicked, R=%d", v);
    }
}




/* 把开关状态 + 三个滑动条的 RGB 值写到板载 WS2812（需持显示锁调用） */
static void led_apply(void)
{
    bool on = g_led_switch && lv_obj_has_state(g_led_switch, LV_STATE_CHECKED);
    if (!on) {
        bsp_rgb_led_clear();
        return;
    }
    uint8_t r = lv_slider_get_value(g_rgb_sliders[0]);
    uint8_t g = lv_slider_get_value(g_rgb_sliders[1]);
    uint8_t b = lv_slider_get_value(g_rgb_sliders[2]);
    bsp_rgb_led_set_rgb(r, g, b);
}

/* 滑动条回调：更新数值标签，并实时刷新 LED 颜色（user_data = 对应数值标签） */
static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *val_label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_label_set_text_fmt(val_label, "%d", (int)lv_slider_get_value(slider));
    led_apply();
}

static void led_switch_event_cb(lv_event_t *e)
{
    (void)e;
    led_apply();
    ESP_LOGI(TAG, "LED switch toggled");
}



/* 创建学习界面：标题 + 计数器按钮 + RGB 滑动条 + LED 开关 */
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

    /* RGB 滑动条：R/G/B 三行，拖动即改板载 WS2812 颜色（开关打开时生效） */
    const char *rgb_names[3] = {"R", "G", "B"};
    const uint32_t rgb_colors[3] = {0xE53935, 0x43A047, 0x1E88E5};
    const uint8_t rgb_inits[3] = {16, 0, 24};
    for (int i = 0; i < 3; i++) {
        const lv_coord_t y = 115 + i * 42; /* 与屏幕中心的垂直偏移 */

        /* 通道名（左侧） */
        lv_obj_t *name = lv_label_create(scr);
        lv_label_set_text(name, rgb_names[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(rgb_colors[i]), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_obj_align(name, LV_ALIGN_CENTER, -230, y);

        /* 当前数值（右侧，作为 user_data 传给回调） */
        lv_obj_t *val = lv_label_create(scr);
        lv_label_set_text_fmt(val, "%d", rgb_inits[i]);
        lv_obj_set_style_text_color(val, lv_color_hex(rgb_colors[i]), 0);
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_align(val, LV_ALIGN_CENTER, 250, y);

        /* 滑动条本体：指示条和旋钮染成对应通道色 */
        g_rgb_sliders[i] = lv_slider_create(scr);
        lv_obj_set_width(g_rgb_sliders[i], 320);
        lv_slider_set_range(g_rgb_sliders[i], 0, 255);
        lv_slider_set_value(g_rgb_sliders[i], rgb_inits[i], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(g_rgb_sliders[i], lv_color_hex(rgb_colors[i]), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(g_rgb_sliders[i], lv_color_hex(rgb_colors[i]), LV_PART_KNOB);
        lv_obj_align(g_rgb_sliders[i], LV_ALIGN_CENTER, 30, y);
        lv_obj_add_event_cb(g_rgb_sliders[i], slider_event_cb, LV_EVENT_VALUE_CHANGED, val);
    }

    /* LED 开关 */
    lv_obj_t *led_label = lv_label_create(scr);
    lv_label_set_text(led_label, "LED");
    lv_obj_set_style_text_color(led_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(led_label, &lv_font_montserrat_20, 0);
    lv_obj_align(led_label, LV_ALIGN_CENTER, -120, 245);

    g_led_switch = lv_switch_create(scr);
    lv_obj_set_size(g_led_switch, 100, 50);
    lv_obj_align(g_led_switch, LV_ALIGN_CENTER, 60, 245);
    lv_obj_add_event_cb(g_led_switch, led_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* IO28 翻转按钮：放在 LED 开关同一行的右侧 */
    lv_obj_t *io28_btn = lv_btn_create(scr);
    lv_obj_set_size(io28_btn, 200, 60);
    lv_obj_align(io28_btn, LV_ALIGN_CENTER, 270, 245);
    lv_obj_add_event_cb(io28_btn, io28_btn_event_cb, LV_EVENT_CLICKED, NULL);

    g_io28_btn_label = lv_label_create(io28_btn);
    lv_label_set_text_fmt(g_io28_btn_label, "IO28: %d", g_io28_level);
    lv_obj_set_style_text_font(g_io28_btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(g_io28_btn_label);

    /* PWM 输出行：放在屏幕最左侧空余竖条（x 0~270 全高度空闲），
     * 三个元素纵向对齐 RGB 滑动条那几行的水平线，橙色区分 */
    const uint32_t pwm_color = 0xFB8C00;

    lv_obj_t *pwm_name = lv_label_create(scr);
    lv_label_set_text(pwm_name, "PWM");
    lv_obj_set_style_text_color(pwm_name, lv_color_hex(pwm_color), 0);
    lv_obj_set_style_text_font(pwm_name, &lv_font_montserrat_20, 0);
    lv_obj_align(pwm_name, LV_ALIGN_CENTER, -360, 115);

    g_pwm_slider = lv_slider_create(scr);
    lv_obj_set_width(g_pwm_slider, 230);
    lv_slider_set_range(g_pwm_slider, 0, 100);
    lv_slider_set_value(g_pwm_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_pwm_slider, lv_color_hex(pwm_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_pwm_slider, lv_color_hex(pwm_color), LV_PART_KNOB);
    lv_obj_align(g_pwm_slider, LV_ALIGN_CENTER, -360, 157);

    g_pwm_val_label = lv_label_create(scr);
    lv_label_set_text(g_pwm_val_label, "0%  0.00V");
    lv_obj_set_style_text_color(g_pwm_val_label, lv_color_hex(pwm_color), 0);
    lv_obj_set_style_text_font(g_pwm_val_label, &lv_font_montserrat_20, 0);
    lv_obj_align(g_pwm_val_label, LV_ALIGN_CENTER, -360, 199);

    lv_obj_add_event_cb(g_pwm_slider, pwm_slider_event_cb, LV_EVENT_VALUE_CHANGED, g_pwm_val_label);

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

        /* 若用户没开 LED 开关，就让板载灯缓慢闪烁，证明任务在跑。
         * 注意：WS2812 还会被 LVGL 回调（led_apply）写，所以闪烁必须
         * 持同一把显示锁，否则两个任务并发进 RMT 驱动会概率性跑飞。 */
        bsp_display_lock(0);
        bool sw_on = g_led_switch && lv_obj_has_state(g_led_switch, LV_STATE_CHECKED);
        if (!sw_on) {
            led_on = !led_on;
            if (led_on) {
                bsp_rgb_led_set_rgb(16, 0, 24); // 暗紫色
            } else {
                bsp_rgb_led_clear();
            }
        }
        bsp_display_unlock();

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

    /* 3.5 IO28：配成普通输出，初始低电平（BSP 未占用该脚） */
    const gpio_config_t io28_conf = {
        .pin_bit_mask = 1ULL << IO28_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io28_conf));
    ESP_ERROR_CHECK(gpio_set_level(IO28_GPIO, 0));
    ESP_LOGI(TAG, "IO28 ready as output (initial low)");

    /* 3.6 PWM 输出：LEDC 20 kHz，占空比由界面滑动条控制（GPIO9 空闲未占用） */
    pwm_init();

    /* 4. 建立 LVGL 界面 */
    ui_create_ui();

    /* 5. 后台任务（示例：打印内存 + 闪烁 LED） */
    xTaskCreate(board_task, "board_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "FreeRTOS + LVGL demo started");
}
