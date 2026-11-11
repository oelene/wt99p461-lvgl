
idf_component_register(
    SRCS "wt99p461_s1_board.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_gpio esp_driver_i2c led_strip
    PRIV_REQUIRES esp_hal_lcd esp_lcd_jd9165 esp_driver_ledc esp_lcd
)
