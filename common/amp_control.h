#pragma once

#include "esp_err.h"

/*
 * Amplifier power control is deliberately asynchronous. RTSP/session code only
 * changes an atomic reference count and wakes a dedicated low-priority task.
 * GPIO access and the disconnect grace delay live exclusively in that task.
 */
esp_err_t amp_control_init(void);
void amp_control_session_connected(void);
void amp_control_session_disconnected(void);
