cmake_minimum_required(VERSION 3.16)

# This folder is the Gateway project: `idf.py build flash` here builds the
# Gateway. Its own code is in main/; common/ holds the code shared with the
# speaker firmware (the project in speaker/), components/ the menuconfig
# options of both.

# Read VERSION as configuration data only.
# Do NOT register it as a CMake/Ninja configure dependency: on ESP-IDF/Ninja
# that can cause an endless "Re-running CMake" loop on some setups.
# If VERSION is edited in an existing build tree, run `idf.py reconfigure` once.

# Single source of truth for the firmware version.
# Change only VERSION (for example v2.0.0 -> v2.0.1).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" AIRPLAY_PROJECT_VERSION)
string(STRIP "${AIRPLAY_PROJECT_VERSION}" AIRPLAY_PROJECT_VERSION)

if(NOT AIRPLAY_PROJECT_VERSION MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z]+)*$")
    message(FATAL_ERROR
        "Invalid VERSION '${AIRPLAY_PROJECT_VERSION}'. Expected e.g. v2.0.0 or v2.0.1-beta1")
endif()

# The Gateway role comes from sdkconfig.defaults.gateway, on top of the
# settings shared with the speakers.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.gateway")
endif()

# ESP-IDF stores PROJECT_VER in esp_app_desc_t::version.
set(PROJECT_VER "${AIRPLAY_PROJECT_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project name is also the generated .bin basename:
# build/airplay-gateway_vX.Y.Z.bin, never to be confused with a speaker's.
project("airplay-gateway_${AIRPLAY_PROJECT_VERSION}")
