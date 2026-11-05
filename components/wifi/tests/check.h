/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdio.h>
#include <string.h>

extern int check_failures;
extern int check_count;

#define CHECK(cond)                                                  \
  do {                                                               \
    check_count++;                                                   \
    if (!(cond)) {                                                   \
      check_failures++;                                              \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
              #cond);                                                \
    }                                                                \
  } while (0)

#define CHECK_INT(a, b)                                                   \
  do {                                                                    \
    long long a_ = (long long)(a), b_ = (long long)(b);                   \
    check_count++;                                                        \
    if (a_ != b_) {                                                       \
      check_failures++;                                                   \
      fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__,     \
              __LINE__, #a, a_, b_);                                      \
    }                                                                     \
  } while (0)

#define CHECK_STR(a, b)                                                 \
  do {                                                                  \
    const char *a_ = (a), *b_ = (b);                                    \
    check_count++;                                                      \
    if (strcmp(a_, b_) != 0) {                                          \
      check_failures++;                                                 \
      fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, \
              __LINE__, #a, a_, b_);                                    \
    }                                                                   \
  } while (0)
