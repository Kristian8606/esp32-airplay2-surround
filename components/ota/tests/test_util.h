/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_failures;
extern int g_checks;

#define CHECK(cond) do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) do { \
        long long a_ = (long long)(a), b_ = (long long)(b); \
        g_checks++; \
        if (a_ != b_) { \
            g_failures++; \
            printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, a_, b_); \
        } \
    } while (0)

#define RUN(fn) do { printf("%s\n", #fn); fflush(stdout); fn(); } while (0)

#ifndef EXAMPLES_DIR
#define EXAMPLES_DIR "examples"
#endif

#define EXAMPLE_LEN 512

/* Reads tests/examples/<name> (512 bytes) into buf. Exits on error. */
static inline void load_example(const char *name, uint8_t buf[EXAMPLE_LEN])
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", EXAMPLES_DIR, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL || fread(buf, 1, EXAMPLE_LEN, f) != EXAMPLE_LEN) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
}

static inline int test_summary(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
