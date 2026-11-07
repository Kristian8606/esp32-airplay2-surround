# SPDX-License-Identifier: Apache-2.0
# helpers: session events, bound sockets and the captive DNS.
# Host tests: cmake -S tests -B build-tests && cmake --build build-tests
idf_component_register(SRCS "src/rtsp_events.c" "src/socket_utils.c"
                            "src/dns_message.c" "src/dns_server.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_common
                       PRIV_REQUIRES lwip log freertos heap)
