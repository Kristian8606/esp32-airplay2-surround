/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* Listener list for session events. The only platform part is the lock, so
 * the same file builds on ESP-IDF and on a computer for the host tests. */

#include "rtsp_events.h"

#include <string.h>

#define RTSP_EVENTS_MAX_LISTENERS 8

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"

/* A spinlock works across both cores and is held only to copy the list. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LIST_LOCK() taskENTER_CRITICAL(&s_lock)
#define LIST_UNLOCK() taskEXIT_CRITICAL(&s_lock)
#else
#include <pthread.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LIST_LOCK() pthread_mutex_lock(&s_lock)
#define LIST_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

typedef struct {
  rtsp_event_callback_t callback;
  void *user_data;
} listener_t;

static listener_t s_listeners[RTSP_EVENTS_MAX_LISTENERS];
static size_t s_count;

int rtsp_events_register(rtsp_event_callback_t callback, void *user_data) {
  if (callback == NULL) {
    return -1;
  }

  int result = -1;
  LIST_LOCK();
  for (size_t i = 0; i < s_count; i++) {
    if (s_listeners[i].callback == callback &&
        s_listeners[i].user_data == user_data) {
      result = 0;
      goto out;
    }
  }
  if (s_count < RTSP_EVENTS_MAX_LISTENERS) {
    s_listeners[s_count].callback = callback;
    s_listeners[s_count].user_data = user_data;
    s_count++;
    result = 0;
  }
out:
  LIST_UNLOCK();
  return result;
}

void rtsp_events_emit(rtsp_event_t event) {
  listener_t snapshot[RTSP_EVENTS_MAX_LISTENERS];
  size_t count;

  /* Callbacks run on a copy without the lock held, so they may emit or
   * register themselves. */
  LIST_LOCK();
  count = s_count;
  memcpy(snapshot, s_listeners, count * sizeof(snapshot[0]));
  LIST_UNLOCK();

  for (size_t i = 0; i < count; i++) {
    snapshot[i].callback(event, snapshot[i].user_data);
  }
}
