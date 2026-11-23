#include "gw_cover_thumbnail.h"

#include <stdbool.h>
#include <stdlib.h>

#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"

#define THUMB_EDGE 768U
#define MAX_SOURCE_EDGE 2048U

typedef struct {
  uint32_t stride, rows;
  uint32_t width, height;
  uint32_t scale_16;
} thumbnail_geometry_t;

static bool thumbnail_geometry(const jpeg_decode_picture_info_t *info,
                               thumbnail_geometry_t *g) {
  const uint32_t w = info->width, h = info->height;
  if (!w || !h || w > MAX_SOURCE_EDGE || h > MAX_SOURCE_EDGE) return false;
  const unsigned mcu_w = info->sample_method == JPEG_DOWN_SAMPLING_YUV444 ? 8 : 16;
  const unsigned mcu_h = info->sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
  if (info->sample_method != JPEG_DOWN_SAMPLING_YUV444 &&
      info->sample_method != JPEG_DOWN_SAMPLING_YUV422 &&
      info->sample_method != JPEG_DOWN_SAMPLING_YUV420) return false;
  g->stride = (w + mcu_w - 1U) / mcu_w * mcu_w;
  g->rows = (h + mcu_h - 1U) / mcu_h * mcu_h;
  const uint32_t edge = w > h ? w : h;
  if (edge <= THUMB_EDGE) return false; /* Keep the original when it already fits. */
  /* The PPA truncates the scale in sixteenths, then floors the output size. */
  g->scale_16 = THUMB_EDGE * 16U / edge;
  g->width = w * g->scale_16 / 16U;
  g->height = h * g->scale_16 / 16U;
  /* The RGB888 to YUV420 DMA transfer uses 32-pixel rows of 16-pixel
   * macroblocks. A partial tile produced an invalid scan on the P4.
   * Serve the original when the resized image cannot fill whole tiles. */
  return g->width > 0 && g->height > 0 &&
         g->width % 32U == 0 && g->height % 16U == 0;
}

uint8_t *gw_cover_thumbnail(const uint8_t *jpeg, uint32_t length,
                            uint32_t output_limit, uint32_t *output_size) {
  jpeg_decode_picture_info_t info;
  thumbnail_geometry_t g;
  if (jpeg_decoder_get_info(jpeg, length, &info) != ESP_OK ||
      !thumbnail_geometry(&info, &g)) return NULL;

  const jpeg_decode_memory_alloc_cfg_t decode_mem = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  size_t decoded_capacity = 0;
  uint8_t *decoded = jpeg_alloc_decoder_mem((size_t)g.stride * g.rows * 3U,
                                          &decode_mem, &decoded_capacity);
  uint8_t *scaled = NULL, *encoded = NULL;
  jpeg_decoder_handle_t decoder = NULL;
  jpeg_encoder_handle_t encoder = NULL;
  ppa_client_handle_t ppa = NULL;
  bool ok = false;
  if (!decoded) goto out;
  const jpeg_decode_engine_cfg_t dec_engine = {.timeout_ms = 350};
  if (jpeg_new_decoder_engine(&dec_engine, &decoder) != ESP_OK) goto out;
  /* ESP-IDF's JPEG encoder takes BGR24 bytes for its RGB888 mode. */
  const jpeg_decode_cfg_t dec = {.output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
                                 .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                                 .conv_std = JPEG_YUV_RGB_CONV_STD_BT601};
  uint32_t decoded_size = 0;
  const esp_err_t dec_result = jpeg_decoder_process(decoder, &dec, jpeg, length, decoded,
                                                     decoded_capacity, &decoded_size);
  if (dec_result != ESP_OK || decoded_size != g.stride * g.rows * 3U) goto out;
  jpeg_del_decoder_engine(decoder);
  decoder = NULL;

  const jpeg_encode_memory_alloc_cfg_t raw_mem = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
  size_t scaled_capacity = 0;
  const uint32_t scaled_size = g.width * g.height * 3U;
  scaled = jpeg_alloc_encoder_mem(scaled_size, &raw_mem, &scaled_capacity);
  if (!scaled) goto out;
  const ppa_client_config_t client = {.oper_type = PPA_OPERATION_SRM};
  if (ppa_register_client(&client, &ppa) != ESP_OK) goto out;
  const ppa_srm_oper_config_t resize = {
      .in = {.buffer = decoded, .pic_w = g.stride, .pic_h = g.rows,
             .block_w = info.width, .block_h = info.height, .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
      .out = {.buffer = scaled, .buffer_size = scaled_capacity, .pic_w = g.width,
              .pic_h = g.height, .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
      .scale_x = g.scale_16 / 16.0f, .scale_y = g.scale_16 / 16.0f,
      .mode = PPA_TRANS_MODE_BLOCKING,
  };
  if (ppa_do_scale_rotate_mirror(ppa, &resize) != ESP_OK) goto out;
  ppa_unregister_client(ppa);
  ppa = NULL;
  free(decoded);
  decoded = NULL;

  const jpeg_encode_memory_alloc_cfg_t encoded_mem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  size_t encoded_capacity = 0;
  encoded = jpeg_alloc_encoder_mem(output_limit, &encoded_mem, &encoded_capacity);
  if (!encoded) goto out;
  const jpeg_encode_engine_cfg_t enc_engine = {.timeout_ms = 350};
  if (jpeg_new_encoder_engine(&enc_engine, &encoder) != ESP_OK) goto out;
  const jpeg_encode_cfg_t enc = {.width = g.width, .height = g.height,
                                 .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
                                 .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = 88};
  ok = jpeg_encoder_process(encoder, &enc, scaled, scaled_size, encoded,
                           encoded_capacity, output_size) == ESP_OK &&
       *output_size >= 4 && *output_size <= output_limit;

out:
  if (encoder) jpeg_del_encoder_engine(encoder);
  if (ppa) ppa_unregister_client(ppa);
  if (decoder) jpeg_del_decoder_engine(decoder);
  free(scaled);
  free(decoded);
  if (ok) return encoded;
  free(encoded);
  return NULL;
}
