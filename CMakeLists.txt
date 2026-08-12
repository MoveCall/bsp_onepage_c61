idf_component_register(
    SRCS "board_c61.c" "board_keys.c" "board_keys_moui.c" "board_mic.c"
    INCLUDE_DIRS "include"
    REQUIRES moui driver esp_adc fatfs esp_driver_sdspi sdmmc esp_driver_i2s esp_wifi esp_netif esp_event bt esp_hw_support espressif__button
)
