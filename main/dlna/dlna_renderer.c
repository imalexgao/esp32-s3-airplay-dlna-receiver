#include "dlna/dlna_renderer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio/audio_output.h"
#include "dlna/dlna_stream.h"
#include "dlna/source_arbiter.h"

static const char *TAG = "dlna_renderer";

static dlna_state_t s_state = DLNA_STATE_STOPPED;
static char s_uri[DLNA_URI_MAX] = {0};
static char s_metadata[DLNA_META_MAX] = {0};

/* Playback position clock (M1: virtual clock; M2 replaces with stream pos). */
static double s_pos_base = 0.0;   /* position at the last transition */
static int64_t s_play_start_us = 0; /* esp_timer time when PLAY began */

/* TEMP DIAG */
static volatile uint32_t s_seturi_calls = 0;
static volatile uint32_t s_play_calls = 0;
static volatile uint32_t s_stream_play_calls = 0;
static volatile uint32_t s_uri_len = 0;

uint32_t dlna_renderer_get_seturi_calls(void) { return s_seturi_calls; }
uint32_t dlna_renderer_get_play_calls(void) { return s_play_calls; }
uint32_t dlna_renderer_get_stream_play_calls(void) { return s_stream_play_calls; }
uint32_t dlna_renderer_get_uri_len(void) { return s_uri_len; }

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
  s_seturi_calls++;
  if (uri && uri[0]) {
    snprintf(s_uri, sizeof(s_uri), "%s", uri);
  } else {
    s_uri[0] = '\0';
  }
  s_uri_len = strlen(s_uri);
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
  s_play_calls++;
  dlna_cp(30); /* play entry */
  bool was_paused = (s_state == DLNA_STATE_PAUSED);
  s_pos_base = clock_now();
  clock_start();
  s_state = DLNA_STATE_PLAYING;
  /* Last-writer-wins: DLNA now owns the output. */
  source_arbiter_activate_dlna();
  dlna_cp(31); /* arbiter done */
  if (s_uri[0]) {
    if (was_paused) {
      dlna_stream_resume();
    } else {
      dlna_stream_play(s_uri);
      s_stream_play_calls++;
      dlna_cp(32); /* stream play returned */
    }
  }
  ESP_LOGI(TAG, "DLNA PLAY (source arbiter: dlna active)");
}

void dlna_renderer_pause(void) {
  if (s_state == DLNA_STATE_PLAYING) {
    s_pos_base = clock_now();
  }
  s_state = DLNA_STATE_PAUSED;
  source_arbiter_release_dlna();
  dlna_stream_pause();
  ESP_LOGI(TAG, "DLNA PAUSE (output released)");
}

void dlna_renderer_stop(void) {
  s_pos_base = 0.0;
  s_state = DLNA_STATE_STOPPED;
  source_arbiter_release_dlna();
  dlna_stream_stop();
  ESP_LOGI(TAG, "DLNA STOP (output released)");
}

void dlna_renderer_seek(double seconds) {
  s_pos_base = seconds < 0.0 ? 0.0 : seconds;
  if (s_state == DLNA_STATE_PLAYING) {
    clock_start();
  }
  dlna_stream_seek(seconds);
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
  /* M2: prefer the stream clock when the transport is running. */
  if (dlna_stream_is_playing()) {
    return dlna_stream_get_position();
  }
  return clock_now();
}

double dlna_renderer_get_duration(void) {
  /* M2: report the stream duration when known. */
  double d = dlna_stream_get_duration();
  if (d > 0.0) {
    return d;
  }
  return 0.0;
}

void dlna_renderer_set_volume(int percent) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  /* Device volume is -30..0 dB; 0% -> -30 dB, 100% -> 0 dB.  The DLNA volume
   * is external/app-controlled, so it goes through the clamped API: it may
   * lower the gain but can never exceed the user's web-slider ceiling. */
  float db = -30.0f + 30.0f * (float)percent / 100.0f;
  audio_output_set_device_volume_db_limited(db);
  ESP_LOGI(TAG, "DLNA SetVolume %d%% -> %.1f dB (clamped)", percent, db);
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
  /* Full teardown, not a soft pause: AirPlay preemption must free the DRAM
   * that the DLNA pull task / decoder holds (QQ Music's multi-connection
   * session already shrinks free DRAM, and keeping the decoder alive pushed
   * free heap down to ~21 KB with a 7.5 KB largest block — too small for the
   * 8 KB AirPlay receiver task stack, so preemption failed with
   * "Failed to create receiver task"). A STOPPED transport is also the
   * behaviour the user wants for a preempted source: it disconnects cleanly
   * instead of lingering in PAUSED. */
  s_pos_base = 0.0;
  s_state = DLNA_STATE_STOPPED;
  ESP_LOGI(TAG, "DLNA preempt: calling dlna_stream_stop()");
  /* Stop the pull task first, then release the output, then flush residual
   * DLNA PCM so AirPlay does not start over stale audio. */
  dlna_stream_stop();
  ESP_LOGI(TAG, "DLNA preempt: stream_stop queued, delaying 150ms");
  /* dlna_stream_stop() is async (a CMD into the stream task); give it a moment
   * to close the HTTP client and free the decoder buffers so the AirPlay
   * receiver task (8 KB stack, DRAM-allocated) can be created. */
  vTaskDelay(pdMS_TO_TICKS(150));
  ESP_LOGI(TAG, "DLNA preempt: releasing source arbiter");
  source_arbiter_release_dlna();
  ESP_LOGI(TAG, "DLNA preempt: flushing output");
  audio_output_flush();
  ESP_LOGI(TAG, "DLNA preempted by AirPlay -> STOPPED (resources released)");
}
