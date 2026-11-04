#include "rtsp_events.h"

#define MAX_LISTENERS 4

static struct {
  rtsp_event_callback_t callback;
  void *user_data;
} listeners[MAX_LISTENERS];

int rtsp_events_register(rtsp_event_callback_t callback, void *user_data) {
  if (!callback) {
    return -1;
  }
  for (int i = 0; i < MAX_LISTENERS; i++) {
    if (listeners[i].callback == callback) {
      return 0; /* already registered */
    }
    if (!listeners[i].callback) {
      listeners[i].callback = callback;
      listeners[i].user_data = user_data;
      return 0;
    }
  }
  return -1;
}

void rtsp_events_emit(rtsp_event_t event) {
  for (int i = 0; i < MAX_LISTENERS && listeners[i].callback; i++) {
    listeners[i].callback(event, listeners[i].user_data);
  }
}
