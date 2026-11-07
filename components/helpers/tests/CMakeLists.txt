# SPDX-FileCopyrightText: 2026 Kristian8606
# SPDX-License-Identifier: Apache-2.0
#
# Host tests (no ESP-IDF):
#   cmake -S tests -B build-tests && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(helpers_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Reads past the datagram are caught by a guard page in the DNS test, so the
# sanitizers are optional (AddressSanitizer hangs at start-up on some macOS
# versions).
option(HELPERS_SANITIZE "Build the tests with ASan and UBSan" OFF)
add_compile_options(-Wall -Wextra -Werror -g)
if(HELPERS_SANITIZE)
  set(SAN -fsanitize=address,undefined -fno-omit-frame-pointer
          -fno-sanitize-recover=all)
  add_compile_options(${SAN})
  add_link_options(${SAN})
endif()

find_package(Threads REQUIRED)
enable_testing()

add_executable(test_dns_message test_dns_message.c ${ROOT}/src/dns_message.c)
target_include_directories(test_dns_message PRIVATE ${ROOT}/src)
add_test(NAME dns_message COMMAND test_dns_message)

add_executable(test_rtsp_events test_rtsp_events.c ${ROOT}/src/rtsp_events.c)
target_include_directories(test_rtsp_events PRIVATE ${ROOT}/include)
target_link_libraries(test_rtsp_events PRIVATE Threads::Threads)
add_test(NAME rtsp_events COMMAND test_rtsp_events)

add_executable(test_socket_utils test_socket_utils.c ${ROOT}/src/socket_utils.c)
target_include_directories(test_socket_utils PRIVATE ${ROOT}/include)
add_test(NAME socket_utils COMMAND test_socket_utils)
