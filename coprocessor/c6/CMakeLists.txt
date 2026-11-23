cmake_minimum_required(VERSION 3.16)

# Firmware of the ESP32-C6 next to an ESP32-P4 Gateway: the Wi-Fi radio
# (ESP-Hosted co-processor over SDIO) and, later, the clock of the time chain.
# It is a project of its own: `idf.py build` in this folder. The P4 Gateway
# build embeds a prebuilt image of it; the S3 builds never use it.

# The version of this firmware only (the Gateway has its own ../../VERSION).
# If VERSION is edited in an existing build tree, run `idf.py reconfigure` once.
file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" C6_FW_VERSION)
string(STRIP "${C6_FW_VERSION}" C6_FW_VERSION)
if(NOT C6_FW_VERSION MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z]+)*$")
    message(FATAL_ERROR "Invalid VERSION '${C6_FW_VERSION}'. Expected e.g. v0.1.0")
endif()

# esp_app_desc_t: project name "airplay-gw-c6" and this version.
set(PROJECT_VER "${C6_FW_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airplay-gw-c6)
