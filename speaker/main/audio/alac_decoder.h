#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct alac_decoder alac_decoder_t;

typedef struct {
  int sample_rate;
  int channels;
  int bits_per_sample;
  int frame_size;
} alac_decoder_config_t;

typedef struct {
  int channels;
  int sample_rate;
  int bits_per_sample;
} alac_decode_info_t;

alac_decoder_t *alac_decoder_create(const alac_decoder_config_t *config);
void alac_decoder_destroy(alac_decoder_t *decoder);
int alac_decoder_decode(alac_decoder_t *decoder, const uint8_t *input,
                        size_t input_len, int16_t *output,
                        size_t output_capacity_frames,
                        alac_decode_info_t *info);
int alac_decoder_decode_float(alac_decoder_t *decoder, const uint8_t *input,
                              size_t input_len, float *output,
                              size_t output_capacity_frames,
                              alac_decode_info_t *info);
