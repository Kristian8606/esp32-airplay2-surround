# SPDX-License-Identifier: Apache-2.0
# Host tests of mdns_airplay with fake mdns_*, settings, Wi-Fi and identity
# functions.
#
#   make        builds with the undefined-behaviour sanitizer and the stack
#               protector, and runs the tests (on macOS under Guard Malloc,
#               which puts a guard page after each heap block)
#   make asan   the same with the address sanitizer, where it works

CC ?= cc
WARN := -std=gnu17 -g -Wall -Wextra -Werror -Wshadow -Wformat=2
# airplay_identity.h is in the firmware's common/, settings.h in
# components/settings.
CPPFLAGS += -Ifakes -I../../include -I../../src/mdns -I../../../../common \
            -I../../../settings/include \
            -I../../../wifi/include

BUILD := build
SRCS := ../../src/mdns/mdns_airplay.c ../../src/mdns/mdns_airplay_txt.c fakes/fakes.c \
        test_mdns_airplay.c
HDRS := $(wildcard ../../include/*.h ../../src/mdns/*.h fakes/*.h)

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
RUN_ENV := DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1
endif

.PHONY: test asan clean
test: $(BUILD)/test_mdns_airplay
	$(RUN_ENV) ./$(BUILD)/test_mdns_airplay

asan: $(BUILD)/test_mdns_airplay_asan
	ASAN_OPTIONS=detect_stack_use_after_return=1 ./$(BUILD)/test_mdns_airplay_asan

$(BUILD)/test_mdns_airplay: $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARN) -O1 -D_FORTIFY_SOURCE=2 -fstack-protector-all \
	  -fsanitize=undefined -fno-sanitize-recover=all -o $@ $(SRCS)

$(BUILD)/test_mdns_airplay_asan: $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARN) -O1 -fno-omit-frame-pointer \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
