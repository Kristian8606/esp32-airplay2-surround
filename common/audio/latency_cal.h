#pragma once
/*
 * Wired output-latency measurement (ADC loopback).
 *
 * The ESP plays a short known chirp through I2S and records what comes back
 * from the DAC/DSP line output on one ADC1 pin. Emission times come from the
 * I2S DMA completion timestamps (the same reference the sync servo uses), the
 * arrival times from the ADC stream, both on esp_timer. No PTP, no iPhone.
 *
 * Nothing here runs unless the user presses "Measure" in the web UI.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LATENCY_CAL_BURSTS        8
#define LATENCY_CAL_ADC_RATE_HZ   80000U
#define LATENCY_CAL_MAX_LATENCY_US 80000   /* search window after emission */

typedef struct {
  bool ok;
  int32_t latency_us;       /* median of the accepted bursts */
  int32_t spread_us;        /* max - min of the accepted bursts */
  uint8_t valid_bursts;
  float corr;               /* median normalised correlation (0..1) */
  int32_t amplitude;        /* peak-to-peak ADC counts of the loud burst */
  bool inverted;            /* signal polarity inverted in the chain */
  const char *error;        /* static text when !ok */
  int32_t burst_us[LATENCY_CAL_BURSTS]; /* per burst, INT32_MIN = rejected */
} latency_cal_result_t;

/* ---- pure DSP part (host-testable) ---- */

/* Fill one stereo int16 block with the test chirp starting at sample 0
 * (rest zeros). */
void latency_cal_fill_burst(int16_t *stereo, uint32_t frames,
                            uint32_t sample_rate);

/* Analyse a captured ADC stream.
 *  samples/n        raw ADC values (one channel, in order)
 *  cb_count/cb_t    for each ADC DMA frame: total conversions so far and the
 *                   esp_timer time (us) when that frame completed
 *  emit_us          esp_timer time (us) when each chirp started at the I2S pin
 *                   reference (same reference as the sync servo). */
void latency_cal_analyze(const uint16_t *samples, uint32_t n,
                         const uint32_t *cb_count, const int64_t *cb_t,
                         uint32_t cb_n, const int64_t *emit_us, int n_emit,
                         latency_cal_result_t *res);

esp_err_t latency_cal_capture_start(void);
/* Stops the capture and analyses it against the emission times. */
void latency_cal_capture_finish(const int64_t *emit_us, int n_emit,
                                latency_cal_result_t *res);
