# ap2_core: the AirPlay 2 protocol core (Apache-2.0): pairing, plists and
# audio decryption.
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
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/hap" "src/plist"
    PRIV_REQUIRES nvs_flash mbedtls esp_hw_support
)
