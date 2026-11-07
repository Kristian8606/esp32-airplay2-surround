# SPDX-License-Identifier: Apache-2.0
# web_server: the shared web server of both firmwares. It calls web_role_*(),
# firmware_id(), the audio engine and the embedded pages of the firmware's
# main component, linked with airplay_link_web_server() in
# common/common.cmake. Host tests: cmake -S tests -B build-tests
idf_component_register(SRCS "src/web_server.c" "src/web_helpers.c"
                            "src/web_rules.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_http_server espressif__cjson
                       PRIV_REQUIRES esp_wifi esp_app_format esp_system
                                     esp_timer mbedtls heap log freertos
                                     settings wifi ota log_stream)
