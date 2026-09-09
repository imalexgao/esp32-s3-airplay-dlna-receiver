#include "dlna/dlna_renderer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "audio/audio_output.h"
#include "dlna/source_arbiter.h"

static const char *TAG = "dlna_renderer";

static dlna_state_t s_state = DLNA_STATE_STOPPED;
static char s_uri[DLNA_URI_MAX] = {0};
static char s_metadata[DLNA_META_MAX] = {0};

/* Playback position clock (M1: virtual clock; M2 replaces with stream pos). */
static double s_pos_base = 0.0;   /* position at the last transition */
static int64_t s_play_start_us = 0; /* esp_timer time when PLAY began */

esp_err_t dlna_renderer_init(void) {
  s_state = DLNA_STATE_STOPPED;
  ESP_LOGI(TAG, "DLNA renderer initialized (state=STOPPED)");
  return ESP_OK;
}

static void clock_start(void) {
  s_play_start_us = esp_timer_get_time();
}

static double clock_now(void) {
  if (s_state != DLNA_STATE_PLAYING) {
    return s_pos_base;
  }
  double elapsed = (double)(esp_timer_get_time() - s_play_start_us) / 1e6;
  return s_pos_base + elapsed;
}

void dlna_renderer_set_uri(const char *uri, const char *metadata) {
  if (uri && uri[0]) {
    snprintf(s_uri, sizeof(s_uri), "%s", uri);
  } else {
    s_uri[0] = '\0';
  }
  if (metadata && metadata[0]) {
    snprintf(s_metadata, sizeof(s_metadata), "%s", metadata);
  } else {
    s_metadata[0] = '\0';
  }
  /* Setting a new URI stops the previous transport (UPnP AV semantics). */
  s_state = DLNA_STATE_STOPPED;
  s_pos_base = 0.0;
  ESP_LOGI(TAG, "SetAVTransportURI: %s", s_uri);
}

void dlna_renderer_play(void) {
  s_pos_base = clock_now();
  clock_start();
  s_state = DLNA_STATE_PLAYING;
  /* Last-writer-wins: DLNA now owns the output. */
  source_arbiter_activate_dlna();
  ESP_LOGI(TAG, "DLNA PLAY (source arbiter: dlna active)");
}

void dlna_renderer_pause(void) {
  if (s_state == DLNA_STATE_PLAYING) {
    s_pos_base = clock_now();
  }
  s_state = DLNA_STATE_PAUSED;
  source_arbiter_release_dlna();
  ESP_LOGI(TAG, "DLNA PAUSE (output released)");
}

void dlna_renderer_stop(void) {
  s_pos_base = 0.0;
  s_state = DLNA_STATE_STOPPED;
  source_arbiter_release_dlna();
  ESP_LOGI(TAG, "DLNA STOP (output released)");
}

void dlna_renderer_seek(double seconds) {
  s_pos_base = seconds < 0.0 ? 0.0 : seconds;
  if (s_state == DLNA_STATE_PLAYING) {
    clock_start();
  }
  ESP_LOGI(TAG, "DLNA SEEK to %.1fs", s_pos_base);
}

dlna_state_t dlna_renderer_get_state(void) {
  return s_state;
}

const char *dlna_renderer_get_uri(void) {
  return s_uri;
}

const char *dlna_renderer_get_metadata(void) {
  return s_metadata;
}

double dlna_renderer_get_position(void) {
  return clock_now();
}

double dlna_renderer_get_duration(void) {
  /* M1: unknown until the stream is opened (M2). */
  return 0.0;
}

void dlna_renderer_set_volume(int percent) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  /* Device volume is -30..0 dB; 0% -> -30 dB, 100% -> 0 dB. */
  float db = -30.0f + 30.0f * (float)percent / 100.0f;
  audio_output_set_device_volume_db(db);
  ESP_LOGI(TAG, "DLNA SetVolume %d%% -> %.1f dB", percent, db);
}

int dlna_renderer_get_volume(void) {
  float db = audio_output_get_device_volume_db();
  int pct = (int)((db + 30.0f) / 30.0f * 100.0f + 0.5f);
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  return pct;
}

void dlna_renderer_pause_external(void) {
  if (s_state == DLNA_STATE_PLAYING) {
    s_pos_base = clock_now();
  }
  s_state = DLNA_STATE_PAUSED;
  source_arbiter_release_dlna();
  ESP_LOGI(TAG, "DLNA preempted by AirPlay -> PAUSED (output released)");
}
