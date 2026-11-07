/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* A small check library for the host tests. */

#include <math.h>
#include <stdio.h>

static int g_failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_INT(actual, expected)                                            \
  do {                                                                         \
    long long a_ = (long long)(actual), e_ = (long long)(expected);            \
    if (a_ != e_) {                                                            \
      fprintf(stderr, "%s:%d: %s is %lld, expected %lld\n", __FILE__,          \
              __LINE__, #actual, a_, e_);                                      \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_NEAR(actual, expected, eps)                                      \
  do {                                                                         \
    double a_ = (actual), e_ = (expected);                                     \
    if (!(fabs(a_ - e_) <= (eps))) {                                           \
      fprintf(stderr, "%s:%d: %s is %.6f, expected %.6f\n", __FILE__,          \
              __LINE__, #actual, a_, e_);                                      \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define RUN(test)                                                              \
  do {                                                                         \
    int before_ = g_failures;                                                  \
    test();                                                                    \
    printf("%s %s\n", g_failures == before_ ? "ok  " : "FAIL", #test);         \
  } while (0)
