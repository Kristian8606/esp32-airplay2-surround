# Gateway firmware: the AirPlay receiver the Apple TV plays to (mDNS,
# pairing, RTSP, PTP). It has no decoder and no I2S: gw_proxy.c implements
# the audio_receiver.h API that RTSP calls and forwards every call and the
# raw audio to the speakers.
include("${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake")

set(GATEWAY_SRCS
    "main.c"
    "web_gateway.c"
    "airplay_identity.c"
    "mdns_airplay.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_remote.c"
    "ap2_compat.c"
    "gw_proxy.c"
    "gw_relay.c"
    "gw_fw.c"
    "gw_rt_relay.c"
    "gw_speakers.c"
    "gw_mic.c"
    "gw_calib.c"
)
list(APPEND GATEWAY_SRCS
    "${AIRPLAY_COMMON_DIR}/network/apple_clock.c"
    "${AIRPLAY_COMMON_DIR}/network/apple_clock_core.c")
if(CONFIG_AIRPLAY_HARDWARE_BUTTONS)
    list(APPEND GATEWAY_SRCS "playback_buttons.c")
endif()

idf_component_register(
    SRCS ${AIRPLAY_COMMON_SRCS} ${GATEWAY_SRCS}
    INCLUDE_DIRS "." "rtsp" ${AIRPLAY_COMMON_INCLUDE_DIRS}
    PRIV_REQUIRES ${AIRPLAY_COMMON_PRIV_REQUIRES} esp_driver_i2s esp_partition
        ap2_core
)

airplay_embed_web_pages(index logs speedtest speakers)
