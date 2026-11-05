# SPDX-License-Identifier: Apache-2.0
#
# Host tests. `make` builds and runs both with UBSan; `make guard` runs them
# again under macOS Guard Malloc (a read past a buffer stops the test).
# Where AddressSanitizer works: make SAN='-fsanitize=address,undefined'
CC      ?= cc
SAN     ?= -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS  ?= -std=c11 -g -O1 -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
SRC     := ../src
BUILD   := build
DEFS    := -DEXAMPLES_DIR='"$(CURDIR)/examples"'

all: run

$(BUILD):
	mkdir -p $@

# Only src/ota_image.c, no ESP-IDF headers at all.
$(BUILD)/test_ota_image: test_ota_image.c $(SRC)/ota_image.c $(SRC)/ota_image.h test_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(DEFS) -I$(SRC) test_ota_image.c $(SRC)/ota_image.c -o $@

# src/ota.c against the host stand-ins in fake_idf/.
$(BUILD)/test_ota_http: test_ota_http.c $(SRC)/ota.c $(SRC)/ota_image.c $(SRC)/ota_image.h \
		fake_idf/*.c fake_idf/*.h test_util.h ../include/ota.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(DEFS) -I$(SRC) -I../include -Ifake_idf \
		test_ota_http.c $(SRC)/ota.c $(SRC)/ota_image.c fake_idf/fake_idf.c -o $@

run: $(BUILD)/test_ota_image $(BUILD)/test_ota_http
	./$(BUILD)/test_ota_image
	./$(BUILD)/test_ota_http

# MALLOC_STRICT_SIZE puts the end of each allocation at a guard page.
guard: $(BUILD)/test_ota_image $(BUILD)/test_ota_http
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 ./$(BUILD)/test_ota_image
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 ./$(BUILD)/test_ota_http

clean:
	rm -rf $(BUILD)

.PHONY: all run guard clean
