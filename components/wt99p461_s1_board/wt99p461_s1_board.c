/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WT9932P4C61-TINY 精简 BSP（学习用）
 * （板卡型号；屏幕驱动与 1024x600 JD9165 时序保持不变）
 * 只保留：I2C、MIPI-DSI 显示（JD9165BA）、GT911 触摸、背光、板载 WS2812。
 * 已移除：SD 卡、SPIFFS、音频 codec(I2S/ES8311)、USB Host、以太网 IP101。
 */

#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_jd9165.h"
#include "hal/mipi_dsi_host_ll.h"
#include "bsp/wt99p461_s1_board.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_touch_gt911.h"
#include "bsp_err_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "wt99p461_s1_board";

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_indev_t *disp_indev = NULL;
static bool disp_indev_is_dummy = false;
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

static bool i2c_initialized = false;
static led_strip_handle_t rgb_led = NULL; // 板载 WS2812
static i2c_master_bus_handle_t i2c_handle = NULL;
static bsp_lcd_handles_t disp_handles;
static esp_ldo_channel_handle_t disp_phy_pwr_chan = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_io_handle_t tp_io_handle = NULL;

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
        .flags.enable_internal_pullup = true,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (i2c_initialized && i2c_handle) {
        BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
        i2c_initialized = false;
    }
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return i2c_handle;
}

// Bit number used to represent command and parameter
#define LCD_LEDC_CH CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

esp_err_t bsp_display_brightness_init(void)
{
    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0,
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&LCD_backlight_channel));
    return ESP_OK;
}

esp_err_t bsp_display_brightness_deinit(void)
{
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = 1,
        .deconfigure = 1,
    };
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_pause(LEDC_LOW_SPEED_MODE, 1));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100; // LEDC resolution set to 10bits, thus: 100% = 1023
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));
    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

static esp_err_t bsp_enable_dsi_phy_power(void)
{
    // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &disp_phy_pwr_chan), TAG, "Acquire LDO channel for DPHY failed");
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");

    return ESP_OK;
}

static esp_err_t bsp_display_scan_dir_init(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_LCD_UPDN) | (1ULL << BSP_LCD_SHLR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "LCD scan dir GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_UPDN, 1), TAG, "UPDN level failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_SHLR, 1), TAG, "SHLR level failed");
    ESP_LOGI(TAG, "LCD scan dir: UPDN=%d SHLR=%d (GPIO high -> panel scan flipped 180)",
             BSP_LCD_UPDN, BSP_LCD_SHLR);
    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    bsp_lcd_handles_t handles;
    ret = bsp_display_new_with_handles(config, &handles);

    *ret_panel = handles.panel;
    *ret_io = handles.io;

    return ret;
}

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t disp_panel = NULL;

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "Brightness init failed");
    ESP_RETURN_ON_ERROR(bsp_enable_dsi_phy_power(), TAG, "DSI PHY power failed");
    ESP_RETURN_ON_ERROR(bsp_display_scan_dir_init(), TAG, "LCD scan direction init failed");

    /* create MIPI DSI bus first, it will initialize the DSI PHY as well */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = config->dsi_bus.phy_clk_src,
        .lane_bit_rate_mbps = config->dsi_bus.lane_bit_rate_mbps,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), TAG, "New DSI bus init failed");

    if (config->hdmi_resolution != BSP_HDMI_RES_NONE) {
        ESP_LOGW(TAG, "Please select HDMI in menuconfig, if you want to use it.");
    }

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // we use DBI interface to send LCD commands and parameters
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io), err, TAG, "New panel IO failed");
    mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(0), false);

    ESP_LOGI(TAG, "Install JD9165BA LCD control panel");

    esp_lcd_dpi_panel_config_t dpi_config = JD9165BA_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
    // W700BH018I-30Z bring-up timing: PLL_F240M / 4 = 60 MHz.
    // HFP 192 -> HT = 1024+136+24+192 = 1376; VFP 100 -> VT = 600+21+2+100 = 723.
    // Frame rate = 60 MHz / (1376*723) = 60.31 Hz (panel spec 48-60 Hz, HT <= 1400, VT <= 800).
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_PLL_F240M;
    dpi_config.dpi_clock_freq_mhz = BSP_LCD_DPI_CLOCK_MHZ;
    dpi_config.video_timing.hsync_front_porch = 192;
    dpi_config.video_timing.vsync_front_porch = 100;
    dpi_config.num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS;

    jd9165_vendor_config_t vendor_config = {
        .mipi_config =
            {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
    };
    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .bits_per_pixel = 16,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .reset_gpio_num = BSP_LCD_RST,
        .flags.reset_active_high = 1,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9165(io, &lcd_dev_config, &disp_panel), err, TAG, "New LCD panel JD9165BA failed");
    mipi_dsi_host_ll_dpi_enable_frame_ack(MIPI_DSI_LL_GET_HOST(0), false);
    ESP_GOTO_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(disp_panel), err, TAG, "Enable LCD DMA2D failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp_panel), err, TAG, "LCD panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel), err, TAG, "LCD panel init failed");

    /* Return all handles */
    ret_handles->io = io;
    disp_handles.io = io;
    ret_handles->mipi_dsi_bus = mipi_dsi_bus;
    disp_handles.mipi_dsi_bus = mipi_dsi_bus;
    ret_handles->panel = disp_panel;
    disp_handles.panel = disp_panel;
    ret_handles->control = NULL;
    disp_handles.control = NULL;

    ESP_LOGI(TAG, "Display initialized: JD9165BA %dx%d, lanes=%d, lane_rate=%d Mbps; shared reset GPIO=%d active-high",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_MIPI_DSI_LANE_NUM,
             config->dsi_bus.lane_bit_rate_mbps, BSP_LCD_TOUCH_RST);

    return ret;

err:
    bsp_display_delete();
    return ret;
}

void bsp_display_delete(void)
{
    if (disp_handles.panel) {
        esp_lcd_panel_del(disp_handles.panel);
        disp_handles.panel = NULL;
    }
    if (disp_handles.io) {
        esp_lcd_panel_io_del(disp_handles.io);
        disp_handles.io = NULL;
    }
    if (disp_handles.mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(disp_handles.mipi_dsi_bus);
        disp_handles.mipi_dsi_bus = NULL;
    }

    if (disp_phy_pwr_chan) {
        esp_ldo_release_channel(disp_phy_pwr_chan);
        disp_phy_pwr_chan = NULL;
    }

    bsp_display_brightness_deinit();
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch, ESP_ERR_INVALID_ARG, TAG, "Missing touch handle output");
    *ret_touch = NULL;
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    static esp_lcd_touch_io_gt911_config_t dev_addr = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };
    dev_addr.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels =
            {
                .reset = 1,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        .driver_data = (void *)&dev_addr,
    };
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    /* GT911 straps its address from INT at power-up. The driver selects the address
       during reset from dev_addr, while the panel IO uses tp_io_config.dev_addr,
       so both must agree. Probe first to keep a 0x14-strapped board working. */
    if (i2c_master_probe(i2c_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 50) == ESP_OK) {
        dev_addr.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    }
    ESP_LOGI(TAG, "GT911: SCL=%d SDA=%d addr=0x%02X RST=%d INT=%d",
             BSP_I2C_SCL, BSP_I2C_SDA, tp_io_config.dev_addr,
             BSP_LCD_TOUCH_RST, BSP_LCD_TOUCH_INT);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    esp_err_t ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(tp_io_handle);
        tp_io_handle = NULL;
        *ret_touch = NULL;
    }
    return ret;
}

void bsp_touch_delete(void)
{
    if (tp) {
        esp_lcd_touch_del(tp);
        tp = NULL;
    }
    if (tp_io_handle) {
        esp_lcd_panel_io_del(tp_io_handle);
        tp_io_handle = NULL;
    }
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new_with_handles(&cfg->hw_cfg, &disp_handles));

    uint32_t display_hres = BSP_LCD_H_RES;
    uint32_t display_vres = BSP_LCD_V_RES;

    ESP_LOGI(TAG, "Display resolution %ldx%ld", display_hres, display_vres);

    const lvgl_port_display_cfg_t disp_cfg = {.io_handle = disp_handles.io,
                                              .panel_handle = disp_handles.panel,
                                              .control_handle = disp_handles.control,
                                              .buffer_size = cfg->buffer_size,
                                              .double_buffer = cfg->double_buffer,
                                              .hres = display_hres,
                                              .vres = display_vres,
                                              .monochrome = false,
                                              .rotation =
                                                  {
                                                      .swap_xy = false,
                                                      .mirror_x = false,
                                                      .mirror_y = false,
                                                  },
                                              .flags = {
                                                  .buff_dma = cfg->flags.buff_dma,
                                                  .buff_spiram = cfg->flags.buff_spiram,
                                                  .sw_rotate = false,
                                                  .direct_mode = true,
                                              }};

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags =
            {
                .avoid_tearing = true,
            },
    };

    return lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
}

static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
{
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg =
            {
                .hdmi_resolution = BSP_HDMI_RES_NONE,
                .dsi_bus =
                    {
                        .phy_clk_src = 0,
                        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                    },
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = true,
                .sw_rotate = true,
            },
    };
    return bsp_display_start_with_config(&cfg);
}

/* Preview-only fallback: register a pointer indev that always reports "released". */
static void bsp_dummy_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = 0;
    data->point.y = 0;
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(lvgl_port_init(&cfg->lvgl_port_cfg));

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);

    /* GPIO22 controls the LCD bias supply (AP3012K), not touch VCC_3V3. */
    const gpio_config_t lcd_en_cfg = {
        .pin_bit_mask = 1ULL << BSP_LCD_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&lcd_en_cfg));
    ESP_ERROR_CHECK(gpio_set_level(BSP_LCD_ENABLE, 1));
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_err_t tp_err = bsp_touch_new(NULL, &tp);
    if (tp_err == ESP_OK) {
        BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(disp), NULL);
    } else {
        ESP_LOGW(TAG, "Touch init failed (%s); registering dummy input for UI preview", esp_err_to_name(tp_err));
        tp = NULL;
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = bsp_dummy_touch_read;
        disp_indev = lv_indev_drv_register(&indev_drv);
        disp_indev_is_dummy = (disp_indev != NULL);
        if (disp_indev == NULL) {
            ESP_LOGE(TAG, "Failed to register dummy input device");
        }
    }
    return disp;
}

void bsp_display_stop(lv_display_t *display)
{
    if (disp_indev) {
        if (disp_indev_is_dummy) {
            lv_indev_delete(disp_indev);
            disp_indev_is_dummy = false;
        } else {
            lvgl_port_remove_touch(disp_indev);
        }
        disp_indev = NULL;
    }
    lvgl_port_remove_disp(display);
    lvgl_port_deinit();

    bsp_touch_delete();
    bsp_display_delete();
    bsp_i2c_deinit();
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t bsp_rgb_led_init(void)
{
    if (rgb_led) {
        return ESP_OK;
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = BSP_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led), TAG,
                        "WS2812 RMT init failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(rgb_led), TAG, "WS2812 clear failed");
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d", BSP_RGB_LED);
    return ESP_OK;
}

esp_err_t bsp_rgb_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(rgb_led, ESP_ERR_INVALID_STATE, TAG, "WS2812 not initialized");
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(rgb_led, 0, red, green, blue), TAG, "WS2812 set pixel failed");
    return led_strip_refresh(rgb_led);
}

esp_err_t bsp_rgb_led_clear(void)
{
    ESP_RETURN_ON_FALSE(rgb_led, ESP_ERR_INVALID_STATE, TAG, "WS2812 not initialized");
    return led_strip_clear(rgb_led);
}
