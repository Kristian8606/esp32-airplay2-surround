# SPDX-License-Identifier: Apache-2.0
# wifi: station, setup access point and host name.
idf_component_register(SRCS "src/wifi.c" "src/wifi_ctl.c" "src/wifi_logic.c"
                            "src/wifi_saved.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_wifi
                       PRIV_REQUIRES esp_event esp_netif esp_timer esp_hw_support
                                     heap log freertos helpers settings)
