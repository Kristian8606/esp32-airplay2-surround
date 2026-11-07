# Host tests of the parts of the wifi module that need no ESP-IDF.
#
#   make -C tests            build and run (UndefinedBehaviorSanitizer)
#   make -C tests SAN=address,undefined   with AddressSanitizer (Linux; it
#                            hangs at start-up on some macOS versions)
#   make -C tests guard      macOS: run under Guard Malloc (catches reads
#                            past the end of a buffer)
#   make -C tests stack      frames of the event loop task's code (also
#                            part of the default run); needs python3
CC ?= cc
SAN ?= undefined
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion \
	-Wno-sign-conversion -fsanitize=$(SAN) -fno-sanitize-recover=all \
	-fno-omit-frame-pointer
# idf_stub/ for esp_log.h; components/settings for settings.h
# (src/wifi_saved.c).
CPPFLAGS += -I../src -Istubs -Iidf_stub -I../../settings/include
SRCS = main.c test_logic.c test_ctl.c test_saved.c ../src/wifi_logic.c \
	../src/wifi_ctl.c ../src/wifi_saved.c

all: run stack

wifi_tests: $(SRCS) ../src/wifi_logic.h ../src/wifi_ctl.h ../src/wifi_saved.h \
		check.h stubs/esp_err.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

run: wifi_tests
	./wifi_tests

guard: wifi_tests
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 \
		./wifi_tests

# Builds src/ (wifi.c against idf_stub/) with -fstack-usage and checks the
# event loop task's frames: no recursion, chain <= 1 KiB, frame <= 256 B.
stack:
	python3 -I stack_report.py $(CC)

clean:
	rm -f wifi_tests
	rm -rf wifi_tests.dSYM

.PHONY: all run guard clean stack
