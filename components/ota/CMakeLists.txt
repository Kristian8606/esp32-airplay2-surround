# SPDX-License-Identifier: Apache-2.0
# ota: firmware update from the web page.
idf_component_register(SRCS "src/ota.c" "src/ota_image.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_http_server
                       PRIV_REQUIRES app_update bootloader_support esp_app_format
                                     esp_partition esp_timer heap log)
