#pragma once

#include <stdbool.h>

/**
 * Source arbiter — decides which source owns the speakers at any moment.
 *
 * Rule (user-approved): SINGLE active source, LAST-WRITER-WINS.
 *  - Only one source produces sound at a time.
 *  - Whichever source issues a PLAY (start or resume) wins the output.
 *  - The preempted source is paused (session kept) and can regain the
 *    output by issuing PLAY again.
 *  - A paused/stopped source does not hold the output.
 *
 * AirPlay state is tracked via RTSP events; DLNA state is driven by
 * dlna_renderer. Connection (session up) is NOT ownership — only PLAY is.
 */

typedef enum {
  SOURCE_ARBITER_IDLE = 0, /* no source playing */
  SOURCE_ARBITER_AIRPLAY,
  SOURCE_ARBITER_DLNA,
} source_arbiter_active_t;

/**
 * Init the arbiter and subscribe to AirPlay RTSP events.
 * Call once at startup.
 */
void source_arbiter_init(void);

/**
 * DLNA issued PLAY. If AirPlay is currently playing, it is preempted
 * (DACP playpause sent to the phone) and DLNA takes the output.
 */
void source_arbiter_activate_dlna(void);

/**
 * AirPlay SETUP received.  A fresh AirPlay connection is about to start
 * feeding RTP; if DLNA is still playing, preempt it right away (not only at
 * RECORD/PLAYING) so the two streams never overlap on the USB FIFO — an
 * overlap is heard as a loud pop/crack when AirPlay grabs the output.
 */
void source_arbiter_notify_airplay_setup(void);

/**
 * DLNA paused or stopped — it no longer owns the output.
 */
void source_arbiter_release_dlna(void);

/**
 * Current owner of the output.
 */
source_arbiter_active_t source_arbiter_get_active(void);

/**
 * True if AirPlay is currently in the playing state (RTSP PLAYING).
 */
bool source_arbiter_airplay_playing(void);
