# Code shared by the two firmwares: the amplifier, the firmware id, the
# Surround time fit, sync pulse and calibration chirp. Not a component of its
# own: the Gateway's main/CMakeLists.txt and the speaker's
# speaker/main/CMakeLists.txt include this file and compile these sources
# into their main component, next to their own code. (The audio_receiver.h
# API calls back into code each firmware implements in its own main, so this
# cannot be a library below main.)
set(AIRPLAY_COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}")

set(AIRPLAY_COMMON_SRCS
    "${AIRPLAY_COMMON_DIR}/amp_control.c"
    "${AIRPLAY_COMMON_DIR}/firmware_id.c"
    "${AIRPLAY_COMMON_DIR}/surround/tsf_fit.c"
    "${AIRPLAY_COMMON_DIR}/surround/sync_pulse.c"
    "${AIRPLAY_COMMON_DIR}/surround/cal_chirp.c"
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
    log_stream
    ota
    wifi
    helpers
    led
    settings
    web_server
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

# The web_server component calls the role pages (web_role_*), firmware_id(),
# the audio engine and the embedded pages, which are compiled into this main
# component: give it their headers and link it with main.
# Call after idf_component_register().
function(airplay_link_web_server)
    idf_component_get_property(web_server_lib web_server COMPONENT_LIB)
    target_include_directories(${web_server_lib} PRIVATE
        "${AIRPLAY_COMMON_DIR}" "${AIRPLAY_COMMON_DIR}/audio")
    target_link_libraries(${web_server_lib} PRIVATE ${COMPONENT_LIB})
endfunction()

# The RTSP server in ap2_core calls the audio engine, clock,
# amplifier and identity, which are compiled into this main
# component: give it their headers and link it with main.
# Call after idf_component_register().
function(airplay_link_ap2_core)
    idf_component_get_property(ap2_core_lib ap2_core COMPONENT_LIB)
    target_include_directories(${ap2_core_lib} PRIVATE
        "${AIRPLAY_COMMON_DIR}" "${AIRPLAY_COMMON_DIR}/audio"
        "${AIRPLAY_COMMON_DIR}/network")
    target_link_libraries(${ap2_core_lib} PRIVATE ${COMPONENT_LIB})
endfunction()
