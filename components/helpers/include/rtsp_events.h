/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Session state changes of the AirPlay receiver, told to whoever asked for
 * them (the LED, for one). See spec/spec.md, "rtsp_events". */

typedef enum {
  RTSP_EVENT_CLIENT_CONNECTED,
  RTSP_EVENT_PLAYING,
  RTSP_EVENT_PAUSED,
  RTSP_EVENT_DISCONNECTED,
} rtsp_event_t;

typedef void (*rtsp_event_callback_t)(rtsp_event_t event, void *user_data);

/* 0 when registered (or already registered with the same user_data), -1
 * for a NULL callback or when the list is full. */
int rtsp_events_register(rtsp_event_callback_t callback, void *user_data);

/* Calls every registered callback with the event, in the caller's task. */
void rtsp_events_emit(rtsp_event_t event);
