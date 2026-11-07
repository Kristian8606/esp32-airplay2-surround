# SPDX-License-Identifier: Apache-2.0
# led: the WS2812 status and level LED of both firmwares.
idf_component_register(SRCS "src/led.c" "src/led_core.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES helpers espressif__led_strip freertos log
                                     esp_timer)
