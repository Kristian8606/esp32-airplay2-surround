#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * RTSP event system - Observer pattern for playback state changes.
 * Allows multiple listeners to react to RTSP events without coupling.
 */

typedef enum {
  RTSP_EVENT_CLIENT_CONNECTED,
  RTSP_EVENT_PLAYING,
  RTSP_EVENT_PAUSED,
  RTSP_EVENT_DISCONNECTED,
} rtsp_event_t;

/**
 * Event callback function type.
 * @param event The event that occurred
 * @param user_data Pointer registered with rtsp_events_register()
 */
typedef void (*rtsp_event_callback_t)(rtsp_event_t event, void *user_data);

/**
 * Register a listener for RTSP events.
 * @param callback Function to call when an event occurs
 * @param user_data Pointer passed to callback (can be NULL)
 * @return 0 on success, -1 if max listeners reached
 */
int rtsp_events_register(rtsp_event_callback_t callback, void *user_data);


/**
 * Emit an event to all registered listeners.
 * Called internally by RTSP handlers.
 * @param event The event to emit
 */
void rtsp_events_emit(rtsp_event_t event);

