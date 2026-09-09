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

/** True while the stream task is actively playing. */
bool dlna_stream_is_playing(void);
