# SPDX-License-Identifier: Apache-2.0
#
# Host tests of the web_server component against fakes of esp_http_server,
# ESP-IDF and the firmware.
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
#
cmake_minimum_required(VERSION 3.16)
project(web_server_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# AddressSanitizer hangs at start-up (in its own shadow memory set-up) with
# Apple's runtime on recent macOS, so on Apple it is off by default; there the
# tests also run under libgmalloc, which puts a guard page after every heap
# block (the tests give the code buffers of exact sizes).
if(APPLE)
  set(asan_default OFF)
else()
  set(asan_default ON)
endif()
option(WEB_TESTS_ASAN "Build with AddressSanitizer" ${asan_default})
option(WEB_TESTS_UBSAN "Build with UndefinedBehaviorSanitizer" ON)

# cJSON from the firmware's managed components (there after a firmware build).
set(CJSON_SOURCE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../managed_components/espressif__cjson/cJSON
    CACHE PATH "Directory with cJSON.c and cJSON.h")

set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -g)
set(sanitizers "")
if(WEB_TESTS_ASAN)
  list(APPEND sanitizers address)
endif()
if(WEB_TESTS_UBSAN)
  list(APPEND sanitizers undefined)
endif()
if(sanitizers)
  list(JOIN sanitizers "," sanitizers)
  add_compile_options(-fsanitize=${sanitizers} -fno-omit-frame-pointer
                      -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=${sanitizers})
endif()

add_library(cjson STATIC ${CJSON_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_SOURCE_DIR})
target_compile_options(cjson PRIVATE -w)

# Include order: the component's headers, the fakes of ESP-IDF, then the
# headers of the other components and of the firmware.
set(REPO_DIR ${COMPONENT_DIR}/../..)
set(TEST_INCLUDES
  ${COMPONENT_DIR}/include
  ${COMPONENT_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/fakes
  ${REPO_DIR}/components/settings/include
  ${REPO_DIR}/components/wifi/include
  ${REPO_DIR}/components/ota/include
  ${REPO_DIR}/components/log_stream/include
  ${REPO_DIR}/common
  ${REPO_DIR}/common/audio)

add_executable(test_rules test_rules.c ${COMPONENT_DIR}/src/web_rules.c
               fakes/fake.c)
target_include_directories(test_rules PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_rules PRIVATE cjson)

set(SERVER_SOURCES
  test_server.c
  ${COMPONENT_DIR}/src/web_server.c
  ${COMPONENT_DIR}/src/web_helpers.c
  ${COMPONENT_DIR}/src/web_rules.c
  fakes/fake.c)

# The admin password is a compile-time option: one build without, one with.
add_executable(test_server_nopw ${SERVER_SOURCES})
target_include_directories(test_server_nopw PRIVATE ${TEST_INCLUDES})
target_compile_definitions(test_server_nopw PRIVATE
  CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD=\"\")
target_link_libraries(test_server_nopw PRIVATE cjson)

add_executable(test_server_pw ${SERVER_SOURCES})
target_include_directories(test_server_pw PRIVATE ${TEST_INCLUDES})
target_compile_definitions(test_server_pw PRIVATE
  CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD=\"s3cret\" WEB_TEST_PASSWORD=\"s3cret\")
target_link_libraries(test_server_pw PRIVATE cjson)

enable_testing()
add_test(NAME rules COMMAND test_rules)
add_test(NAME server_no_password COMMAND test_server_nopw)
add_test(NAME server_password COMMAND test_server_pw)
set(tests rules server_no_password server_password)
if(APPLE AND NOT WEB_TESTS_ASAN AND EXISTS /usr/lib/libgmalloc.dylib)
  add_test(NAME rules_gmalloc COMMAND test_rules)
  add_test(NAME server_no_password_gmalloc COMMAND test_server_nopw)
  add_test(NAME server_password_gmalloc COMMAND test_server_pw)
  set_tests_properties(rules_gmalloc server_no_password_gmalloc
    server_password_gmalloc PROPERTIES ENVIRONMENT
    "DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib;MALLOC_STRICT_SIZE=1")
  list(APPEND tests rules_gmalloc server_no_password_gmalloc
       server_password_gmalloc)
endif()
set_tests_properties(${tests} PROPERTIES TIMEOUT 300)
