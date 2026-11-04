#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __APPLE__
#include <stdlib.h>
static inline void esp_fill_random(void *buf, size_t len) { arc4random_buf(buf, len); }
#else
#include <sys/random.h>
static inline void esp_fill_random(void *buf, size_t len) { (void)!getrandom(buf, len, 0); }
#endif
static inline uint32_t esp_random(void) { uint32_t v; esp_fill_random(&v, sizeof(v)); return v; }
