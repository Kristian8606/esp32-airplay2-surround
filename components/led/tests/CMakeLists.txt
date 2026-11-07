# SPDX-FileCopyrightText: 2026 Kristian8606
# SPDX-License-Identifier: Apache-2.0
#
# Host tests of the led component (no ESP-IDF):
#   cmake -S tests -B build-tests && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
# LED_SANITIZE: address (ASan + UBSan, the default), undefined (UBSan only,
# for hosts where the ASan runtime does not start; run the tests with
# DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib on macOS to catch reads
# past a buffer), thread (ThreadSanitizer) or none.

cmake_minimum_required(VERSION 3.16)
project(led_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(LED_SANITIZE "address" CACHE STRING "address, undefined, thread or none")

set(LED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wundef -Werror -g -O1)
if(LED_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(LED_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
  add_link_options(-fsanitize=undefined)
elseif(LED_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

enable_testing()

add_executable(test_core test_core.c ${LED_ROOT}/src/led_core.c)
target_include_directories(test_core PRIVATE ${LED_ROOT}/src ${LED_ROOT}/../helpers/include)
target_link_libraries(test_core PRIVATE Threads::Threads m)
add_test(NAME core COMMAND test_core)

# src/led.c with the fakes, once per menuconfig set.
function(glue_test name)
  cmake_parse_arguments(G "" "" "DEFS;SCENARIOS" ${ARGN})
  add_executable(test_glue_${name} test_glue.c fakes/fake_idf.c
                 ${LED_ROOT}/src/led.c ${LED_ROOT}/src/led_core.c)
  target_include_directories(test_glue_${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes ${LED_ROOT}/include ${LED_ROOT}/src
    ${LED_ROOT}/../helpers/include)
  target_compile_definitions(test_glue_${name} PRIVATE ${G_DEFS})
  # Each menuconfig set uses only some of the helpers.
  target_compile_options(test_glue_${name} PRIVATE -Wno-unused-function)
  target_link_libraries(test_glue_${name} PRIVATE Threads::Threads m)
  foreach(s ${G_SCENARIOS})
    add_test(NAME glue_${name}_${s} COMMAND test_glue_${name} ${s})
  endforeach()
endfunction()

glue_test(vu
  DEFS CONFIG_ENABLE_RGB_AUDIO_LED=1 CONFIG_RGB_AUDIO_LED_GPIO=38
       CONFIG_RGB_AUDIO_LED_BRIGHTNESS=96 CONFIG_RGB_AUDIO_LED_UPDATE_HZ=30
       CONFIG_RGB_AUDIO_LED_PLAYING_VU=1 CONFIG_RGB_AUDIO_LED_PAUSED_STEADY=1
       CONFIG_RGB_AUDIO_LED_STANDBY_OFF=1
  SCENARIOS ok strip_fail task_fail write_fail register_fail)

glue_test(steady
  DEFS CONFIG_ENABLE_RGB_AUDIO_LED=1 CONFIG_RGB_AUDIO_LED_GPIO=5
       CONFIG_RGB_AUDIO_LED_BRIGHTNESS=255 CONFIG_RGB_AUDIO_LED_UPDATE_HZ=30
       CONFIG_RGB_AUDIO_LED_PLAYING_STEADY=1 CONFIG_RGB_AUDIO_LED_PAUSED_OFF=1
       CONFIG_RGB_AUDIO_LED_STANDBY_STEADY=1
  SCENARIOS ok strip_fail task_fail write_fail register_fail)

# Without the LED the other options do not exist either.
glue_test(disabled
  DEFS
  SCENARIOS disabled)

# The build-time checks: src/led.c must stop with its #error.
set(LED_GOOD_OPTIONS
  -DCONFIG_ENABLE_RGB_AUDIO_LED=1 -DCONFIG_RGB_AUDIO_LED_GPIO=38
  -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=96 -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=30)
set(LED_GOOD_CHOICES
  -DCONFIG_RGB_AUDIO_LED_PLAYING_VU=1 -DCONFIG_RGB_AUDIO_LED_PAUSED_STEADY=1
  -DCONFIG_RGB_AUDIO_LED_STANDBY_OFF=1)
function(build_error_test name regex)
  add_test(NAME build_error_${name}
    COMMAND ${CMAKE_C_COMPILER} -std=gnu11 -fsyntax-only
      -I${CMAKE_CURRENT_SOURCE_DIR}/fakes -I${LED_ROOT}/include
      -I${LED_ROOT}/src -I${LED_ROOT}/../helpers/include ${ARGN} ${LED_ROOT}/src/led.c)
  set_tests_properties(build_error_${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

build_error_test(two_playing "exactly one CONFIG_RGB_AUDIO_LED_PLAYING_"
  ${LED_GOOD_OPTIONS} ${LED_GOOD_CHOICES}
  -DCONFIG_RGB_AUDIO_LED_PLAYING_STEADY=1)
build_error_test(no_playing "exactly one CONFIG_RGB_AUDIO_LED_PLAYING_"
  ${LED_GOOD_OPTIONS} -DCONFIG_RGB_AUDIO_LED_PAUSED_STEADY=1
  -DCONFIG_RGB_AUDIO_LED_STANDBY_OFF=1)
build_error_test(two_paused "exactly one CONFIG_RGB_AUDIO_LED_PAUSED_"
  ${LED_GOOD_OPTIONS} ${LED_GOOD_CHOICES}
  -DCONFIG_RGB_AUDIO_LED_PAUSED_OFF=1)
build_error_test(no_standby "exactly one CONFIG_RGB_AUDIO_LED_STANDBY_"
  ${LED_GOOD_OPTIONS} -DCONFIG_RGB_AUDIO_LED_PLAYING_VU=1
  -DCONFIG_RGB_AUDIO_LED_PAUSED_STEADY=1)
build_error_test(gpio_49 "GPIO must be 0-48"
  ${LED_GOOD_CHOICES} -DCONFIG_ENABLE_RGB_AUDIO_LED=1
  -DCONFIG_RGB_AUDIO_LED_GPIO=49 -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=96
  -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=30)
build_error_test(no_gpio "GPIO must be 0-48"
  ${LED_GOOD_CHOICES} -DCONFIG_ENABLE_RGB_AUDIO_LED=1
  -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=96 -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=30)
build_error_test(brightness_256 "BRIGHTNESS must be 0-255"
  ${LED_GOOD_CHOICES} -DCONFIG_ENABLE_RGB_AUDIO_LED=1
  -DCONFIG_RGB_AUDIO_LED_GPIO=38 -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=256
  -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=30)
build_error_test(hz_4 "UPDATE_HZ must be 5-60"
  ${LED_GOOD_CHOICES} -DCONFIG_ENABLE_RGB_AUDIO_LED=1
  -DCONFIG_RGB_AUDIO_LED_GPIO=38 -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=96
  -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=4)
build_error_test(hz_61 "UPDATE_HZ must be 5-60"
  ${LED_GOOD_CHOICES} -DCONFIG_ENABLE_RGB_AUDIO_LED=1
  -DCONFIG_RGB_AUDIO_LED_GPIO=38 -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=96
  -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=61)
# And the edges of the ranges build.
add_test(NAME build_ok_edges
  COMMAND ${CMAKE_C_COMPILER} -std=gnu11 -fsyntax-only -Wundef -Werror
    -I${CMAKE_CURRENT_SOURCE_DIR}/fakes -I${LED_ROOT}/include
    -I${LED_ROOT}/src -I${LED_ROOT}/../helpers/include -DCONFIG_ENABLE_RGB_AUDIO_LED=1
    -DCONFIG_RGB_AUDIO_LED_GPIO=0 -DCONFIG_RGB_AUDIO_LED_BRIGHTNESS=255
    -DCONFIG_RGB_AUDIO_LED_UPDATE_HZ=60 -DCONFIG_RGB_AUDIO_LED_PLAYING_OFF=1
    -DCONFIG_RGB_AUDIO_LED_PAUSED_OFF=1 -DCONFIG_RGB_AUDIO_LED_STANDBY_STEADY=1
    ${LED_ROOT}/src/led.c)
