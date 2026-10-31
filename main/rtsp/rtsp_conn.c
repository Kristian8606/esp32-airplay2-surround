#include "rtsp_conn.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "amp_control.h"
#include "apple_clock.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE volume_lock = portMUX_INITIALIZER_UNLOCKED;

static void rtsp_conn_cleanup(rtsp_conn_t *conn);


static int32_t volume_db_to_q15(float volume_db){
    if (volume_db <= -30.0f)
        return 0;
    if (volume_db >= 0.0f)
        return 32768;
    float normalized = (volume_db + 30.0f) / 30.0f;
    float curved = normalized * normalized;

    return (int32_t)(curved * 32768.0f);
}

rtsp_conn_t *rtsp_conn_create(void) {
  rtsp_conn_t *conn = calloc(1, sizeof(rtsp_conn_t));
  if (!conn) {
    return NULL;
  }

  /* Current AirPlay volume (reported by GET /info) or a conservative
   * default. A connection does not touch the output until it takes the play
   * lock: a sender that merely connects never moves the playing volume. */
  float saved_volume;
  if (settings_get_volume(&saved_volume) == ESP_OK) {
    conn->volume_db = saved_volume;
  } else {
    conn->volume_db = -15.0f;
  }

  conn->event_socket = -1;

  return conn;
}

void rtsp_conn_free(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  /* The RTSP connection owns exactly one amplifier session reference.
   * Stream TEARDOWN intentionally does not touch it: only destruction of the
   * control connection starts the amplifier power-off grace period. */
  if (conn->amp_session_active) {
    conn->amp_session_active = false;
    amp_control_session_disconnected();
  }

  // Persist volume at disconnect (only the session owner changed it)
  if (conn->owns_session) settings_persist_volume();

  // Cleanup any resources
  rtsp_conn_cleanup(conn);

  // Free HAP session if present
  if (conn->hap_session) {
    hap_session_free(conn->hap_session);
    conn->hap_session = NULL;
  }

  free(conn->crypto_scratch);
  /* Give the play lock back only now: everything the owner had in the global
   * modules is gone, so the next owner starts from a clean state. */
  rtsp_session_release(conn);
  free(conn);
}

/* Full cleanup when the connection closes: closes sockets, clears PTP. */
static void rtsp_conn_cleanup(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Note: audio_receiver_stop() is NOT called here — it is a global operation
  // and must be managed by the caller (rtsp_server cleanup / handle_teardown)
  // to avoid killing a new session's audio during client replacement.

  // Close sockets
  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }

  // Reset stream state
  conn->stream_active = false;
  conn->stream_paused = false;
  conn->data_port = 0;
  conn->control_port = 0;
  conn->event_port = 0;
  conn->buffered_port = 0;

  // A probe must not clear the session owner's timing record.
  if (conn->owns_session) {
    apple_clock_release();
  }

  // Reset encryption state
  conn->encrypted_mode = false;
}

static bool conn_update_volume(rtsp_conn_t *conn, float volume_db,
                               bool conditional, float expected_db) {
  if (!conn || !isfinite(volume_db)) return false;
  if (volume_db > 0.0f) volume_db = 0.0f;
  if (volume_db < -144.0f) volume_db = -144.0f;
  int32_t gain = volume_db_to_q15(volume_db);
  portENTER_CRITICAL(&volume_lock);
  if (conditional && conn->volume_db != expected_db) {
    portEXIT_CRITICAL(&volume_lock);
    return false;
  }
  conn->volume_db = volume_db;
  audio_receiver_set_volume_q15(gain); /* Atomic target; existing output ramp. */
  /* This setter only updates the cached float; NVS is written at disconnect. */
  settings_set_volume(volume_db);
  portEXIT_CRITICAL(&volume_lock);
  return true;
}

void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db) {
  conn_update_volume(conn, volume_db, false, 0.0f);
}

bool rtsp_conn_set_volume_if_unchanged(rtsp_conn_t *conn, float expected_db,
                                      float volume_db) {
  return conn_update_volume(conn, volume_db, true, expected_db);
}

void rtsp_conn_take_volume(rtsp_conn_t *conn) {
  if (!conn) return;
  float volume_db;
  const bool known = settings_get_volume(&volume_db) == ESP_OK;
  portENTER_CRITICAL(&volume_lock);
  if (known) conn->volume_db = volume_db;
  audio_receiver_set_volume_q15(volume_db_to_q15(conn->volume_db));
  portEXIT_CRITICAL(&volume_lock);
}

float rtsp_conn_get_volume_db(rtsp_conn_t *conn) {
  if (!conn) return -144.0f;
  portENTER_CRITICAL(&volume_lock);
  float volume = conn->volume_db;
  portEXIT_CRITICAL(&volume_lock);
  return volume;
}

