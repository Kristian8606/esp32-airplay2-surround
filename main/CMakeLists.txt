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
)
if(CONFIG_AIRPLAY_HARDWARE_BUTTONS)
    list(APPEND GATEWAY_SRCS "playback_buttons.c")
endif()

idf_component_register(
    SRCS ${AIRPLAY_COMMON_SRCS} ${GATEWAY_SRCS}
    INCLUDE_DIRS "." ${AIRPLAY_COMMON_INCLUDE_DIRS}
    PRIV_REQUIRES ${AIRPLAY_COMMON_PRIV_REQUIRES} esp_driver_i2s esp_partition
        ap2_core esp_http_client mbedtls
)

airplay_embed_web_pages(index logs speedtest speakers roomeq)
# The Speakers page's radio controls (gw_radio.c serves /radio.js).
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../data/www/radio.js"
    BINARY RENAME_TO "web_radio_js")
airplay_link_ap2_core()
airplay_link_web_server()
