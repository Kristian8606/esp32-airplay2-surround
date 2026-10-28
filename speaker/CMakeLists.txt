cmake_minimum_required(VERSION 3.16)

# Speaker project: `idf.py build flash` in this folder builds the speaker
# firmware. Its own code is in main/ and components/ (FDK AAC) here; the
# code shared with the Gateway is ../common, the menuconfig options are in
# ../components. The role (Speaker) is the Kconfig default.

set(AIRPLAY_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Single source of truth for the firmware version: ../VERSION. If it is
# edited in an existing build tree, run `idf.py reconfigure` once.
file(READ "${AIRPLAY_ROOT}/VERSION" AIRPLAY_PROJECT_VERSION)
string(STRIP "${AIRPLAY_PROJECT_VERSION}" AIRPLAY_PROJECT_VERSION)

if(NOT AIRPLAY_PROJECT_VERSION MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z]+)*$")
    message(FATAL_ERROR
        "Invalid VERSION '${AIRPLAY_PROJECT_VERSION}'. Expected e.g. v2.0.0 or v2.0.1-beta1")
endif()

set(EXTRA_COMPONENT_DIRS "${AIRPLAY_ROOT}/components")

# The settings shared with the Gateway, then this folder's own.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${AIRPLAY_ROOT}/sdkconfig.defaults;sdkconfig.defaults")
endif()

set(PROJECT_VER "${AIRPLAY_PROJECT_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project name is also the generated .bin basename:
# build/airplay-speaker_vX.Y.Z.bin.
project("airplay-speaker_${AIRPLAY_PROJECT_VERSION}")
