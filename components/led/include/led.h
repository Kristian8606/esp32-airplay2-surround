/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The single WS2812 RGB LED of both firmwares: the AirPlay state, and a
 * level meter of the audio while playing. See spec/spec.md. */

/* Starts the LED once, at start-up. Without the LED in menuconfig, or when
 * the LED cannot be set up, it does nothing and later calls do nothing. */
void led_init(void);

/* The final audio just handed to I2S: stereo_frames frames of two 32-bit
 * slots (left, right), the sample in the top 16 bits. Called from the audio
 * output task; returns at once. */
void led_audio_feed(const int32_t *pcm, size_t stereo_frames);

#ifdef __cplusplus
}
#endif
