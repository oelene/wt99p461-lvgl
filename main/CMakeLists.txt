idf_component_register(
    SRCS main.cpp
    INCLUDE_DIRS .
    REQUIRES wt99p461_s1_board lvgl__lvgl nvs_flash esp_driver_gpio
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
)

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB}
        PRIVATE
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()
