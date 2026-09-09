#include "dlna/source_arbiter.h"

#include "esp_log.h"

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
  }
  s_dlna_active = true;
}

void source_arbiter_release_dlna(void) {
  s_dlna_active = false;
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
