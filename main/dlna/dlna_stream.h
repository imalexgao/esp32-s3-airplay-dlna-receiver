#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * DLNA/UPnP media-stream transport (M2).
 *
 * Pulls the URI set by SetAVTransportURI over HTTP, decodes it to interleaved
 * stereo int16 PCM and feeds the shared USB-host output chain via
 * audio_output_usb_host_feed_pcm(). The source arbiter guarantees DLNA is the
 * single active source while this stream is running.
 */

esp_err_t dlna_stream_init(void);

/** Start (or restart) streaming the given URI. */
esp_err_t dlna_stream_play(const char *uri);

/** Pause streaming (keep the HTTP connection; feed stops). */
void dlna_stream_pause(void);

/** Resume a paused stream. */
void dlna_stream_resume(void);

/** Stop streaming and free the connection. */
void dlna_stream_stop(void);

/** Seek to a position in seconds (supported formats only). */
void dlna_stream_seek(double seconds);

/** Current play position in seconds (0 = unknown). */
double dlna_stream_get_position(void);

/** Stream duration in seconds (0 = unknown). */
double dlna_stream_get_duration(void);

/* TEMP DIAG: number of feed_pcm() invocations since boot */
uint32_t dlna_stream_get_feed_count(void);
/* TEMP DIAG: last dlna_stream_play() rc + whether the task object exists */
int dlna_stream_get_last_err(void);
bool dlna_stream_task_alive(void);
/* TEMP DIAG: HTTP open status/err, decoded MP3 frames, feed calls, end code */
int dlna_stream_get_http_status(void);
int dlna_stream_get_http_err(void);
uint32_t dlna_stream_get_mp3_frames(void);
uint32_t dlna_stream_get_mp3_feed_calls(void);
/* TEMP DIAG: FLAC decoded frames + onRead (HTTP) invocations */
uint64_t dlna_stream_get_flac_frames(void);
uint64_t dlna_stream_get_flac_read_calls(void);
int dlna_stream_get_fmt_diag(void);
int dlna_stream_get_flac_open_rc(void);
uint64_t dlna_stream_get_aac_frames(void);
int dlna_stream_get_aac_open_rc(void);
uint32_t dlna_stream_get_aac_feed_calls(void);
int dlna_stream_get_stream_end(void);
/* TEMP DIAG: crash checkpoint surviving restart */
uint32_t dlna_stream_get_crash_stage(void);
/* TEMP DIAG: write a crash checkpoint (survives restart, see dlna_stream.c) */
void dlna_cp(uint32_t s);
uint32_t dlna_stream_get_dec_size(void);
uint32_t dlna_stream_get_scratch_size(void);

/** True while the stream task is actively playing. */
bool dlna_stream_is_playing(void);
