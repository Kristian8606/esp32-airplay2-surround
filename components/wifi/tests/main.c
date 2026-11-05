/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "check.h"

int check_failures;
int check_count;

void test_logic(void);
void test_ctl(void);
void test_saved(void);

int main(void) {
  test_logic();
  test_ctl();
  test_saved();
  printf("%d checks, %d failed\n", check_count, check_failures);
  return check_failures == 0 ? 0 : 1;
}
