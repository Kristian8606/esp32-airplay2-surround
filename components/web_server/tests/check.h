/* SPDX-License-Identifier: Apache-2.0 */
/* A very small test harness. */
#pragma once
#include <stdio.h>
#include <string.h>

extern int check_failures;
extern int check_count;

#define CHECK(cond)                                                     \
  do {                                                                  \
    check_count++;                                                      \
    if (!(cond)) {                                                      \
      check_failures++;                                                 \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,  \
              #cond);                                                   \
    }                                                                   \
  } while (0)

#define CHECK_EQ_INT(a, b)                                                  \
  do {                                                                      \
    long long a_ = (long long)(a), b_ = (long long)(b);                     \
    check_count++;                                                          \
    if (a_ != b_) {                                                         \
      check_failures++;                                                     \
      fprintf(stderr, "%s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, \
              #a, a_, b_);                                                  \
    }                                                                       \
  } while (0)

#define CHECK_STR(a, b)                                                    \
  do {                                                                     \
    const char *a_ = (a), *b_ = (b);                                       \
    check_count++;                                                         \
    if (a_ == NULL || b_ == NULL || strcmp(a_, b_) != 0) {                 \
      check_failures++;                                                    \
      fprintf(stderr, "%s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__,     \
              __LINE__, #a, a_ ? a_ : "(null)", b_ ? b_ : "(null)");       \
    }                                                                      \
  } while (0)

#define RUN(test)                          \
  do {                                     \
    int before_ = check_failures;          \
    test();                                \
    printf("%s %s\n", check_failures == before_ ? "ok  " : "FAIL", #test); \
  } while (0)

#define CHECK_MAIN_END()                                               \
  do {                                                                 \
    printf("%d checks, %d failed\n", check_count, check_failures);     \
    return check_failures == 0 ? 0 : 1;                                \
  } while (0)
