# Gateway firmware: the AirPlay receiver the Apple TV plays to (mDNS,
# pairing, RTSP, PTP). It has no decoder and no I2S: gw_proxy.c implements
# the sound_engine.h API that RTSP calls and forwards every call and the
# raw audio to the speakers.
include("${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake")

set(GATEWAY_SRCS
    "main.c"
    "homekit_reset.c"
    "web_gateway.c"
    "airplay_identity.c"
    "gw_proxy.c"
    "gw_relay.c"
    "gw_fw.c"
    "gw_rt_relay.c"
    "gw_speakers.c"
    "gw_mic.c"
    "gw_calib.c"
    "gw_radio.c"
    "gw_rew.c"
    "gw_local_volume.c"
)
if(CONFIG_GATEWAY_AIRPLAY_ARTWORK)
    list(APPEND GATEWAY_SRCS "gw_airplay_artwork.c")
endif()
if(CONFIG_GATEWAY_PLAYER_BENCH)
    list(APPEND GATEWAY_SRCS "gw_bench.c" "gw_bench_alac_lite.c")
elseif(CONFIG_GATEWAY_PLAYER_TEST_RADIO_LANES OR CONFIG_GATEWAY_USB_PLAYER)
    list(APPEND GATEWAY_SRCS "gw_bench_alac_lite.c")
endif()
if(CONFIG_GATEWAY_USB_PLAYER)
    list(APPEND GATEWAY_SRCS "gw_player.c" "gw_player_decim.c" "gw_player_m4a.c" "gw_player_src.c" "gw_player_tags.c")
endif()
if(CONFIG_GATEWAY_USB_PLAYER_USB)
    list(APPEND GATEWAY_SRCS "gw_usb.c")
    if(IDF_TARGET STREQUAL "esp32p4")
        list(APPEND GATEWAY_SRCS "gw_cover_cache.c" "gw_cover_thumbnail.c")
    endif()
endif()
if(CONFIG_AIRPLAY_HARDWARE_BUTTONS)
    list(APPEND GATEWAY_SRCS "playback_buttons.c")
endif()
# Wi-Fi through an ESP-Hosted co-processor (P4 + C6): the time comes from it.
# Requirements are read before sdkconfig, so esp_hosted (a dependency of the
# P4 only, main/idf_component.yml) goes by target.
if(CONFIG_ESP_HOSTED_HOST)
    list(APPEND GATEWAY_SRCS "gw_c6_time.c" "gw_c6_fw.c")
endif()
set(GATEWAY_TARGET_REQUIRES "")
if(IDF_TARGET STREQUAL "esp32p4")
    set(GATEWAY_TARGET_REQUIRES espressif__esp_hosted esp_driver_jpeg esp_driver_ppa)
endif()

idf_component_register(
    SRCS ${AIRPLAY_COMMON_SRCS} ${GATEWAY_SRCS}
    INCLUDE_DIRS "." ${AIRPLAY_COMMON_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/gw_c6"
    PRIV_REQUIRES ${AIRPLAY_COMMON_PRIV_REQUIRES} esp_driver_i2s esp_partition
        ap2_core esp_driver_gptimer esp_http_client mbedtls ${GATEWAY_TARGET_REQUIRES}
)

airplay_embed_web_pages(index logs speedtest speakers roomeq)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../web/radio.js"
    BINARY RENAME_TO "web_radio_js")
if(CONFIG_AIRPLAY_SURROUND_PLAYER_STREAMS)
    # The REW mode (gw_rew.c) comes with protocol 15 (SURROUND_PROTO_MEASURE),
    # which needs this option.
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../web/rew.js"
        BINARY RENAME_TO "web_rew_js")
endif()
if(CONFIG_GATEWAY_USB_PLAYER)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../web/player.js"
        BINARY RENAME_TO "web_player_js")
endif()

# Embed the matching C6 application for the P4's ESP-Hosted transport.
if(CONFIG_ESP_HOSTED_HOST)
    file(GLOB gw_c6_fw "${CMAKE_CURRENT_LIST_DIR}/../coprocessor/c6/prebuilt/airplay-gw-c6_v*.bin")
    list(LENGTH gw_c6_fw gw_c6_fw_count)
    if(NOT gw_c6_fw_count EQUAL 1)
        message(FATAL_ERROR "coprocessor/c6/prebuilt/ must hold exactly one airplay-gw-c6_v*.bin")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${gw_c6_fw}" BINARY RENAME_TO gw_c6_fw_bin)
endif()
airplay_link_ap2_core()
include("${CMAKE_CURRENT_LIST_DIR}/ap2_formats.cmake")
# Local playback uses the same volume curve as the pinned RTSP core.
target_include_directories(${COMPONENT_LIB} PRIVATE "${ap2_formats_dir}/src/rtsp")
airplay_link_web_server()
