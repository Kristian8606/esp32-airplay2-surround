# SPDX-License-Identifier: Apache-2.0
#
# Host tests of the settings component against a fake NVS.
#   make                       build and run both configurations
#   make SAN=address,undefined also with AddressSanitizer (where it works)
#   make gmalloc               macOS: run under Guard Malloc, every heap
#                              block ends at a guard page
#   make clean

CC ?= cc
# AddressSanitizer hangs at start-up on macOS 26 with Apple clang 17, so the
# default is UBSan only.
SAN ?= undefined
CFLAGS ?= -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wshadow -Wconversion \
          -Wno-sign-conversion -fno-omit-frame-pointer \
          -fsanitize=$(SAN) -fno-sanitize-recover=all
LDFLAGS ?= -fsanitize=$(SAN) -pthread -lm
INCLUDES = -I../include -I../src -Istubs
SRCS = ../src/settings.c ../src/settings_core.c fake_nvs.c test_settings.c
HDRS = $(wildcard ../include/*.h ../src/*.h stubs/*.h stubs/freertos/*.h) fake_nvs.h

# Both firmwares use CONFIG_AIRPLAY_MULTICHANNEL=y; the other build checks
# the -1 default without it.
CONFIG_MC = -DCONFIG_AIRPLAY_OUTPUT_LATENCY_US=-25000 \
            -DCONFIG_AIRPLAY_MULTICHANNEL=1 \
            -DCONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER=3
CONFIG_NO_MC = -DCONFIG_AIRPLAY_OUTPUT_LATENCY_US=0

all: run

build/test_mc: $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) $(CONFIG_MC) $(SRCS) -o $@ $(LDFLAGS)

build/test_no_mc: $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) $(CONFIG_NO_MC) $(SRCS) -o $@ $(LDFLAGS)

run: build/test_mc build/test_no_mc
	./build/test_mc
	./build/test_no_mc

gmalloc: build/test_mc build/test_no_mc
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 \
	  ./build/test_mc
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 \
	  ./build/test_no_mc

clean:
	rm -rf build

.PHONY: all run gmalloc clean
