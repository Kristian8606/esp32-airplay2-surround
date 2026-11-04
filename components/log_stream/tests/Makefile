# SPDX-License-Identifier: Apache-2.0
# Host tests: make -C tests
#   test_core   capture and text logic (src/ls_core.c)
#   test_esp    src/log_stream.c against a fake ESP-IDF (esp_fake.c,
#               esp_stubs/; signatures from the ESP-IDF v6.0.2 API
#               reference, not the real headers)
#   make asan       the same under AddressSanitizer
#   make esp-check  compile-only check of src/log_stream.c
CC ?= cc
WARN = -std=gnu11 -Wall -Wextra -Werror
CFLAGS ?= $(WARN) -O2 -g -fsanitize=undefined -fno-sanitize-recover=undefined
SRC = ../src

ESP_INC = -DLS_HOST_TEST -Iesp_stubs -I../include -I$(SRC)
ESP_SRC = test_esp.c esp_fake.c $(SRC)/log_stream.c $(SRC)/ls_core.c
ESP_DEPS = $(ESP_SRC) esp_fake.h $(SRC)/ls_core.h ../include/log_stream.h $(wildcard esp_stubs/*.h esp_stubs/*/*.h)

all: test
test_core: test_core.c $(SRC)/ls_core.c $(SRC)/ls_core.h
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_core.c $(SRC)/ls_core.c
test_esp: $(ESP_DEPS)
	$(CC) $(CFLAGS) $(ESP_INC) -o $@ $(ESP_SRC)
test: test_core test_esp
	./test_core examples/boot.log
	./test_esp examples/boot.log
asan:
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined -I$(SRC) -o test_core_asan test_core.c $(SRC)/ls_core.c
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined $(ESP_INC) -o test_esp_asan $(ESP_SRC)
	./test_core_asan examples/boot.log
	./test_esp_asan examples/boot.log
esp-check:
	$(CC) -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -Iesp_stubs -I../include -I$(SRC) $(SRC)/log_stream.c
clean:
	rm -rf test_core test_core_asan test_esp test_esp_asan *.dSYM
.PHONY: all test asan esp-check clean
