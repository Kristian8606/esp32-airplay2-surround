/* SPDX-License-Identifier: Apache-2.0 */
#include <sodium.h>

#include "test.h"

void run_unit_tests(void);
void run_core_tests(void);
void run_remote_tests(void);

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  /* On the board ap2_hap_identity_init() starts libsodium (spec "Interface
   * to the firmware"); the host tests start it here, once. */
  if (sodium_init() < 0) {
    printf("libsodium does not start\n");
    return 1;
  }
  run_unit_tests();
  run_core_tests();
  run_remote_tests();
  printf("\n%d checks, %d failed\n", t_checks, t_failures);
  return t_failures == 0 ? 0 : 1;
}
