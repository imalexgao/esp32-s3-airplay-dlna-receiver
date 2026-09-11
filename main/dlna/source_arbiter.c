#include "dlna/source_arbiter.h"

#include "esp_log.h"

#include "audio/audio_output.h"
#include "audio/audio_receiver.h"
#include "dlna/dlna_renderer.h"
#include "dacp_client.h"
#include "rtsp/rtsp_events.h"

static const char *TAG = "source_arbiter";

static bool s_airplay_playing = false;
static bool s_dlna_active = false;

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_PLAYING:
    s_airplay_playing = true;
    /* AirPlay resumed = a fresh PLAY = last-writer-wins: preempt DLNA. */
    if (s_dlna_active) {
      ESP_LOGI(TAG, "AirPlay PLAY preempts active DLNA session");
      dlna_renderer_pause_external();
    }
    break;
  case RTSP_EVENT_PAUSED:
  case RTSP_EVENT_DISCONNECTED:
    s_airplay_playing = false;
    break;
  default:
    break;
  }
}

void source_arbiter_init(void) {
  rtsp_events_register(on_rtsp_event, NULL);
  ESP_LOGI(TAG, "Source arbiter ready (single-active, last-writer-wins)");
}

void source_arbiter_activate_dlna(void) {
  if (s_airplay_playing) {
    ESP_LOGI(TAG, "DLNA PLAY preempts active AirPlay session");
    /* Ask the phone to pause (same path as the hardware pause button). */
    dacp_send_playpause();
    /* Hard-stop AirPlay playout on the device regardless of whether the
     * sender acknowledged the DACP pause.  Some senders (e.g. 网易云) do not
     * expose a reachable DACP endpoint, so the phone would otherwise keep
     * feeding audio and the two streams would mix on the USB FIFO.  The RTSP
     * session stays up; a later RECORD resumes AirPlay normally. */
    audio_receiver_set_playing(false);
    /* Fully stop the RTP receiver as well: the phone (e.g. 网易云, no DACP
     * endpoint) keeps pushing encrypted audio, and decrypting/queueing it on
     * the device steals CPU and WiFi from the DLNA HTTP stream, which the user
     * hears as the DLNA track slowing and stuttering.  The RTSP session stays
     * up; a later RECORD re-starts the receiver normally. */
    audio_receiver_stop();
    audio_output_flush();
  }
  s_dlna_active = true;
  /* Park the AirPlay playback task so it stops touching the shared
   * resampler/FIFO while DLNA feeds the same chain from its own task. */
  audio_output_set_dlna_active(true);
}

void source_arbiter_notify_airplay_setup(void) {
  if (s_dlna_active) {
    ESP_LOGI(TAG, "AirPlay SETUP preempts active DLNA session");
    dlna_renderer_pause_external();
  }
}

void source_arbiter_release_dlna(void) {
  s_dlna_active = false;
  /* Let the AirPlay playback task back in; it re-syncs the shared resampler
   * from source_rate on its next loop. */
  audio_output_set_dlna_active(false);
}

source_arbiter_active_t source_arbiter_get_active(void) {
  if (s_dlna_active) {
    return SOURCE_ARBITER_DLNA;
  }
  if (s_airplay_playing) {
    return SOURCE_ARBITER_AIRPLAY;
  }
  return SOURCE_ARBITER_IDLE;
}

bool source_arbiter_airplay_playing(void) {
  return s_airplay_playing;
}
