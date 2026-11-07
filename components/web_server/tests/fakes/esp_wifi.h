/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);
