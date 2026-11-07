# SPDX-License-Identifier: Apache-2.0
# Host tests of the parts of the RTSP server that need no ESP-IDF.
#   make                          build and run (UBSan)
#   make SAN=address,undefined    with AddressSanitizer too (Linux; it hangs
#                                 at start-up on some macOS versions)
#   make clean

CC ?= cc
SRC := ../../src/rtsp
# The firmware's own headers (audio engine, clock, amplifier, identity) are
# in common/; the session events are in components/helpers, the settings in
# components/settings.
FW := ../../../../common
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter
SAN ?= undefined
CFLAGS += -fsanitize=$(SAN) -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS += -I../../include -I../../../helpers/include -I../../../settings/include -I$(FW) -I$(FW)/audio -I$(FW)/network -I$(SRC) -Istubs -DCONFIG_AIRPLAY_LOG_RTSP_REQUESTS
# libsodium (spec "Placement"), linked with -lsodium. Its place comes from
# pkg-config, else Homebrew, else the compiler's default paths.
SODIUM_PREFIX := $(shell brew --prefix libsodium 2>/dev/null)
ifneq ($(shell pkg-config --exists libsodium 2>/dev/null && echo y),)
SODIUM_CFLAGS ?= $(shell pkg-config --cflags libsodium)
SODIUM_LIBS ?= $(shell pkg-config --libs libsodium)
else ifneq ($(SODIUM_PREFIX),)
SODIUM_CFLAGS ?= -I$(SODIUM_PREFIX)/include
SODIUM_LIBS ?= -L$(SODIUM_PREFIX)/lib -lsodium
else
SODIUM_LIBS ?= -lsodium
endif
CFLAGS += $(SODIUM_CFLAGS)
LDLIBS := $(SODIUM_LIBS) -lm

SOURCES := $(SRC)/rtsp_frame.c $(SRC)/rtsp_msg.c \
           $(SRC)/rtsp_tables.c $(SRC)/rtsp_core.c $(SRC)/rtsp_remote.c \
           fakes.c client.c test_units.c test_core.c test_remote.c main.c

.PHONY: all test clean
all: test

rtsp_tests: $(SOURCES) $(wildcard $(SRC)/*.h) $(wildcard *.h) $(wildcard ../../include/*.h)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

test: rtsp_tests
	./rtsp_tests

clean:
	rm -rf rtsp_tests rtsp_tests.dSYM
