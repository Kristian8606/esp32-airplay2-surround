# ap2_core: the AirPlay 2 protocol core (Apache-2.0): pairing, plists,
# audio decryption, the FairPlay setup replies, the RTSP server and the
# mDNS announcement. The RTSP server and the announcement call the firmware
# (audio engine, settings, clock, amplifier, identity): the firmware gives
# its headers and links it with airplay_link_ap2_core() in
# common/common.cmake. Session events come from the helpers component.
idf_component_register(
    SRCS
        "src/hap/hap.c"
        "src/hap/hap_crypto.c"
        "src/hap/hap_pair_setup.c"
        "src/hap/hap_pair_verify.c"
        "src/hap/srp.c"
        "src/hap/tlv8.c"
        "src/plist/plist_xml.c"
        "src/plist/bplist_parser.c"
        "src/plist/bplist_builder.c"
        "src/plist/base64.c"
        "src/audio_crypto.c"
        "src/fairplay/fairplay.c"
        "src/fairplay/fairplay_tables.c"
        "src/rtsp/rtsp_core.c"
        "src/rtsp/rtsp_frame.c"
        "src/rtsp/rtsp_msg.c"
        "src/rtsp/rtsp_remote.c"
        "src/rtsp/rtsp_server.c"
        "src/rtsp/rtsp_tables.c"
        "src/mdns/mdns_airplay.c"
        "src/mdns/mdns_airplay_txt.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/hap" "src/plist" "src/fairplay" "src/rtsp" "src/mdns"
    PRIV_REQUIRES nvs_flash mbedtls esp_hw_support lwip esp_timer helpers
                  wifi espressif__mdns
)
