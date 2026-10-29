#pragma once

#include <stddef.h>

/* "v4.5.5 7c5d1e74": the version from VERSION, which stays the same from
 * build to build, plus the start of this firmware's ELF SHA256, which does
 * not. Tells boards apart that were updated with different builds. */
void firmware_id(char *out, size_t len);
