#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * DLNA/UPnP AV renderer state machine.
 *
 * M1 scope: full control-plane state (STOPPED/PLAYING/PAUSED), current URI
 * and DIDL metadata, a playback-position clock, and volume wired to the
 * device volume (same slider as the web UI). Audio transport is added in M2.
 */

#define DLNA_URI_MAX 1024
#define DLNA_META_MAX 2048

typedef enum {
  DLNA_STATE_STOPPED = 0,
  DLNA_STATE_PLAYING,
  DLNA_STATE_PAUSED,
} dlna_state_t;

esp_err_t dlna_renderer_init(void);

/* Control actions (from SOAP). */
void dlna_renderer_set_uri(const char *uri, const char *metadata);
void dlna_renderer_play(void);
void dlna_renderer_pause(void);
void dlna_renderer_stop(void);
void dlna_renderer_seek(double seconds);

/* State queries. */
dlna_state_t dlna_renderer_get_state(void);
const char *dlna_renderer_get_uri(void);
const char *dlna_renderer_get_metadata(void);
double dlna_renderer_get_position(void);
double dlna_renderer_get_duration(void);

/* TEMP DIAG */
uint32_t dlna_renderer_get_seturi_calls(void);
uint32_t dlna_renderer_get_play_calls(void);
uint32_t dlna_renderer_get_stream_play_calls(void);
uint32_t dlna_renderer_get_uri_len(void);

/* Volume, 0..100, wired to the device volume (web UI slider). */
void dlna_renderer_set_volume(int percent);
int dlna_renderer_get_volume(void);

/**
 * Preempted by AirPlay (source arbiter): an active DLNA session is paused
 * while its URI/metadata/position are kept, so the control point can resume.
 */
void dlna_renderer_pause_external(void);
