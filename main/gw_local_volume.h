#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The saved master level for USB and radio. AirPlay keeps its own setting. */
void gw_local_volume_init(void);
int32_t gw_local_volume_get(void);
int32_t gw_local_volume_apply(void); /* when a local stream opens */
esp_err_t gw_local_volume_set(int32_t q15, bool save);
bool gw_local_volume_step(bool up); /* consumes a button press if local audio owns the speakers */

int32_t gw_local_volume_from_percent(double percent);
int gw_local_volume_percent(int32_t q15);

/* Compatibility with the existing dB requests and volume_db responses. */
#define GW_LOCAL_VOLUME_KEEP_DB 1
int32_t gw_local_volume_from_db(int db);
int gw_local_volume_db(int32_t q15);
