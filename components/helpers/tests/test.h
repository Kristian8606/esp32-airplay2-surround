/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* A minimal check macro for the host tests. */

#include <stdio.h>
#include <stdlib.h>

static int test_failures;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
              #cond);                                                     \
      test_failures++;                                                    \
    }                                                                     \
  } while (0)

#define TEST_RESULT()                                                     \
  (test_failures == 0 ? (printf("%s: all passed\n", __FILE__), 0)         \
                      : (printf("%s: %d failed\n", __FILE__, test_failures), \
                         1))
