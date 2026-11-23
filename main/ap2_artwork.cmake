# Optional artwork handling shares the Gateway's build-tree RTSP sources.
list(APPEND ap2_gateway_sources src/rtsp/rtsp_msg.c src/plist/bplist_parser.c)
list(APPEND ap2_gateway_patches ap2_artwork.patch)
target_include_directories(${ap2_formats_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
