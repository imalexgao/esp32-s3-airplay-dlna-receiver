#include "rtsp_conn.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "ptp_clock.h"
#include "settings.h"

rtsp_conn_t *rtsp_conn_create(void) {
  rtsp_conn_t *conn = calloc(1, sizeof(rtsp_conn_t));
  if (!conn) {
    return NULL;
  }

  /* AirPlay (phone/source) session volume: starts at 0 dB (full) and is
   * controlled by the source during the session via RTSP SET_PARAMETER. It is
   * deliberately independent from the device volume (settings): the phone
   * volume must NOT move the web slider and must NOT overwrite the persisted
   * device volume. See rtsp_conn_set_volume(). */
  conn->volume_db = 0.0f; /* 0 dB = full */
  conn->volume_q15 = 32768;

  conn->data_socket = -1;
  conn->control_socket = -1;
  conn->event_socket = -1;

  return conn;
}

void rtsp_conn_free(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  /* The device volume (web slider / buttons) is persisted by its own setters
   * (playback_control_set_volume / settings_persist_volume). The phone volume
   * is session-scoped and must not be persisted here — it would overwrite the
   * user's device volume with the source's last volume. */

  // Cleanup any resources
  rtsp_conn_cleanup(conn);

  // Free HAP session if present
  if (conn->hap_session) {
    hap_session_free(conn->hap_session);
    conn->hap_session = NULL;
  }

  free(conn);
}

void rtsp_conn_reset_stream(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Reset stream state but keep session alive
  conn->stream_active = false;
  conn->stream_paused = true; // Paused, not fully torn down

  // Keep ports allocated for quick resume
  // Don't clear: data_port, control_port, timing_port, event_port
}

void rtsp_conn_cleanup(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Note: audio_receiver_stop() is NOT called here — it is a global operation
  // and must be managed by the caller (rtsp_server cleanup / handle_teardown)
  // to avoid killing a new session's audio during client replacement.

  // Close sockets
  if (conn->data_socket >= 0) {
    close(conn->data_socket);
    conn->data_socket = -1;
  }
  if (conn->control_socket >= 0) {
    close(conn->control_socket);
    conn->control_socket = -1;
  }
  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }

  // Reset stream state
  conn->stream_active = false;
  conn->stream_paused = false;
  conn->data_port = 0;
  conn->control_port = 0;
  conn->timing_port = 0;
  conn->event_port = 0;
  conn->buffered_port = 0;

  // Clear PTP clock for fresh sync on next connection
  ptp_clock_clear();

  // Reset encryption state
  conn->encrypted_mode = false;
}

void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db) {
  if (!conn) {
    return;
  }

  conn->volume_db = volume_db;

  // AirPlay volume: 0 dB = max, -30 dB = mute
  // Use squared curve for better perceptual control
  if (volume_db <= -30.0f) {
    conn->volume_q15 = 0;
  } else if (volume_db >= 0.0f) {
    conn->volume_q15 = 32768;
  } else {
    // Map -30..0 to 0..1, then square for perceptual curve
    float normalized = (volume_db + 30.0f) / 30.0f;
    float curved = normalized * normalized;
    conn->volume_q15 = (int32_t)(curved * 32768.0f);
  }

  /* NOTE: the AirPlay/source volume is deliberately NOT written to settings —
   * settings now holds the independent DEVICE volume (web slider / buttons),
   * and the source's volume must not move or overwrite it. */
}

int32_t rtsp_conn_get_volume_q15(rtsp_conn_t *conn) {
  if (!conn) {
    return 32768; // Default full volume
  }
  return conn->volume_q15;
}
