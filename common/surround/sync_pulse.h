#pragma once

#include "esp_err.h"

/* Sync test (CONFIG_AIRPLAY_SURROUND_SYNC_PULSE): raise a GPIO at every whole
 * second of the sender's clock, as ptp_clock_get_snapshot() maps it to this
 * ESP's clock: from PTP on the Gateway, from the Gateway's TIME messages on a
 * Surround speaker. Two boards' rising edges on an oscilloscope differ by
 * their clock error; audio adds its own playout error on top (measure that
 * on the DAC outputs). A no-op when the option is off. */
esp_err_t sync_pulse_start(void);
