# SPDX-License-Identifier: Apache-2.0
# log_stream: the live log on /ws/logs. Host tests: make -C tests
idf_component_register(SRCS "src/log_stream.c" "src/ls_core.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_http_server
                       PRIV_REQUIRES log heap freertos)
