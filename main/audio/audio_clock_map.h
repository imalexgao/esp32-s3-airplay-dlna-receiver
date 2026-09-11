#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The anchor lives in the SENDER's clock domain, which is PTP for AirPlay 2
 * and NTP for AirPlay 1.  The mapping is identical either way, so nothing
 * here names a protocol; the caller converts local time into the domain the
 * anchor arrived in before calling audio_clock_map_network_to_rtp(). */
typedef struct {
  bool valid;
  uint32_t sample_rate;
  uint32_t anchor_rtp;
  uint64_t anchor_network_ns;
  int64_t playout_offset_ns;
  /* 65536 = 1.0.  Self-bootstrapped anchors (senders that never send
   * SETRATEANCHORTIME) map local time into the sender's RTP domain, but the
   * sender's RTP clock can run a fraction of a percent faster or slower than
   * the board's local clock.  A fixed 1.0 slope then makes playout error
   * accumulate every second, the drift servo saturates and the stream
   * stutters.  Re-bootstrap measures the actual RTP arrival rate between two
   * anchor points and stores the ratio here so the mapping slope follows the
   * sender instead of fighting it. */
  int32_t rate_scale_q16;
  /* True when the current mapping comes from a REAL sender anchor (RTP
   * control packets 0x54/0x57, or RTSP SETRATEANCHORTIME).  False when it was
   * self-bootstrapped (senders that never send an anchor).  The re-bootstrap
   * loop must only re-align SELF-bootstrapped mappings: with a real anchor the
   * sender keeps re-arming the clock map itself, and re-bootstrap fighting it
   * every 2 s is what turns a healthy stream into a sawtooth stutter (drift
   * saturates +-20000 ppm while concealed stays 0). */
  bool sender_anchored;
} audio_clock_map_t;

void audio_clock_map_reset(audio_clock_map_t *map);
bool audio_clock_map_set(audio_clock_map_t *map, uint32_t sample_rate,
                         uint32_t anchor_rtp, uint64_t anchor_network_ns,
                         int64_t playout_offset_ns);
/* Override the mapping slope after a self-bootstrap.  q16 fixed point,
 * 65536 = 1.0 (the default set by audio_clock_map_set). */
void audio_clock_map_set_rate_scale(audio_clock_map_t *map, int32_t q16);
/* Mark the mapping as sender-anchored (real 0x54/0x57/SETRATEANCHORTIME) or
 * self-bootstrapped.  Re-bootstrap only acts on self-bootstrapped maps. */
void audio_clock_map_set_sender_anchored(audio_clock_map_t *map, bool anchored);
bool audio_clock_map_rtp_to_network(const audio_clock_map_t *map, uint32_t rtp,
                                    int64_t *network_ns);
bool audio_clock_map_network_to_rtp(const audio_clock_map_t *map,
                                    int64_t network_ns, uint32_t *rtp);
