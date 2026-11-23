idf_component_register(SRCS "main.c" "c6_clock.c" "c6_pulse.c" "c6_tsf.c" "c6_ptp.c" "c6_link.c"
                       PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../common/surround"
                                         "${CMAKE_CURRENT_LIST_DIR}/../../../common/gw_c6"
                       PRIV_REQUIRES esp_hosted nvs_flash esp_event esp_app_format esp_driver_gpio esp_wifi
                       REQUIRES esp_driver_gptimer)
