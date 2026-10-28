# Code shared by the two firmwares: Wi-Fi, settings, web server core, OTA,
# logs, the clock state and the Surround sync protocol. Not a component of
# its own: the Gateway's main/CMakeLists.txt and the speaker's
# speaker/main/CMakeLists.txt include this file and compile these sources
# into their main component, next to their own code. (The web server and the
# audio_receiver.h API call back into code each firmware implements in its
# own main, so this cannot be a library below main.)
set(AIRPLAY_COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}")

set(AIRPLAY_COMMON_SRCS
    "${AIRPLAY_COMMON_DIR}/amp_control.c"
    "${AIRPLAY_COMMON_DIR}/settings.c"
    "${AIRPLAY_COMMON_DIR}/led.c"
    "${AIRPLAY_COMMON_DIR}/rtsp_events.c"
    "${AIRPLAY_COMMON_DIR}/network/wifi.c"
    "${AIRPLAY_COMMON_DIR}/network/ptp_clock.c"
    "${AIRPLAY_COMMON_DIR}/network/ptp_clock_engine.c"
    "${AIRPLAY_COMMON_DIR}/network/socket_utils.c"
    "${AIRPLAY_COMMON_DIR}/network/web_server.c"
    "${AIRPLAY_COMMON_DIR}/network/ota.c"
    "${AIRPLAY_COMMON_DIR}/network/dns_server.c"
    "${AIRPLAY_COMMON_DIR}/network/log_stream.c"
    "${AIRPLAY_COMMON_DIR}/surround/tsf_fit.c"
    "${AIRPLAY_COMMON_DIR}/surround/sync_pulse.c"
)

# Diagnostics are a true compile-time feature. When both diagnostic
# categories that use it (PLAYOUT, LIFECYCLE) are off, audio_diag.c is not compiled.
if(CONFIG_AIRPLAY_DIAG_ACTIVE)
    list(APPEND AIRPLAY_COMMON_SRCS "${AIRPLAY_COMMON_DIR}/audio/audio_diag.c")
endif()

set(AIRPLAY_COMMON_INCLUDE_DIRS
    "${AIRPLAY_COMMON_DIR}"
    "${AIRPLAY_COMMON_DIR}/audio"
    "${AIRPLAY_COMMON_DIR}/network"
    "${AIRPLAY_COMMON_DIR}/surround"
)

set(AIRPLAY_COMMON_PRIV_REQUIRES
    esp_driver_gpio
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    esp_http_server
    app_update
    esp_app_format
    mbedtls
)

# Embed web pages (data/www/<page>.html) into the OTA application image.
# File changes automatically regenerate the embedded data; no separate
# filesystem upload is needed. Call after idf_component_register().
function(airplay_embed_web_pages)
    foreach(page ${ARGN})
        target_add_binary_data(${COMPONENT_LIB}
            "${AIRPLAY_COMMON_DIR}/../data/www/${page}.html" BINARY
            RENAME_TO "web_${page}_html")
    endforeach()
endfunction()
