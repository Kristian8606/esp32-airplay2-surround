#include "firmware_id.h"

#include <stdio.h>

#include "esp_app_desc.h"

void firmware_id(char *out, size_t len) {
  if (!out || !len) return;
  const esp_app_desc_t *app = esp_app_get_description();
  char sha[9] = {0};
  (void)esp_app_get_elf_sha256(sha, sizeof(sha));
  snprintf(out, len, "%s %s", app ? app->version : "?", sha);
}
