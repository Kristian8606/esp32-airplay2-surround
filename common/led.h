#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_out_sample.h"

/* RGB/VU diagnostic LED. When CONFIG_ENABLE_RGB_AUDIO_LED=n all public
 * functions compile to tiny no-op stubs in led.c and no RMT/WS2812 work is
 * performed from the audio path. */
void led_init(void);
/* Final I2S samples (16- or 32-bit slots); the VU uses the top 16 bits. */
void led_audio_feed(const audio_out_sample_t *pcm, size_t stereo_frames);
