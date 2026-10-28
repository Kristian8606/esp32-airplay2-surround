# Speaker firmware: follows the Gateway and plays one channel through I2S.
# It is not an AirPlay receiver (no RTSP, pairing or PTP). Its own code is
# the audio engine (audio/), the link to the Gateway (spk_link.c,
# spk_time.c) and its pages (web_speaker.c).
include("${CMAKE_CURRENT_LIST_DIR}/../../common/common.cmake")

set(SPEAKER_SRCS
    "main.c"
    "web_speaker.c"
    "audio/audio_receiver.c"
    "audio/ap2_buffered_fifo.c"
    "audio/audio_eq.c"
    "audio/audio_loudness.c"
    "audio/aac_decoder.c"
    "audio/alac_decoder.c"
    "audio/realtime_receiver.c"
    "audio/pcm_fifo.c"
    "audio/pcm_rtp_ring.c"
    "audio/audio_playout.c"
    "audio/audio_crypto.c"
    "spk_link.c"
    "spk_time.c"
)
# Output latency: the pure analysis is always built (tiny), the ADC
# capture only with the wired measurement option.
list(APPEND SPEAKER_SRCS "audio/latency_cal_dsp.c")
if(CONFIG_AIRPLAY_LATENCY_CAL)
    list(APPEND SPEAKER_SRCS "audio/latency_cal.c")
endif()

idf_component_register(
    SRCS ${AIRPLAY_COMMON_SRCS} ${SPEAKER_SRCS}
    INCLUDE_DIRS "." "audio" ${AIRPLAY_COMMON_INCLUDE_DIRS}
    PRIV_REQUIRES
        ${AIRPLAY_COMMON_PRIV_REQUIRES}
        esp_driver_i2s
        esp_adc
        fdk_aac
)

airplay_embed_web_pages(index logs speedtest eq)
