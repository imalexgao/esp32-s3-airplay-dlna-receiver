/**
 * USB Audio Class HOST output — native UAC 2.0/1.0 isochronous-OUT streamer.
 *
 * The ESP32-S3 acts as a USB host and streams decoded AirPlay PCM to a
 * connected USB-C headphone/DAC.  Espressif's usb_host_uac component only
 * supports UAC 1.0, but common devices (e.g. Apple USB-C EarPods) are UAC 2.0,
 * so this backend talks to the native ESP-IDF USB Host library directly:
 *
 *   - registers a USB host client
 *   - on device connect: parses the active config descriptor, finds an audio
 *     STREAMING interface alt-setting that is 16-bit stereo on an isochronous
 *     OUT endpoint (works for UAC 1.0 and UAC 2.0 — we don't parse the class
 *     header, just the standard AS interface/format/endpoint descriptors)
 *   - claims that interface+alt (which issues SET_INTERFACE)
 *   - streams PCM via a pool of isochronous OUT transfers, kept continuously
 *     fed from a PCM FIFO that the playback task fills (silence on underrun).
 *
 * The connected device's endpoint is assumed synchronous (no feedback EP); we
 * send the nominal 48 kHz data rate. Sample-rate is the compile-time
 * the device-supported rate (44.1 kHz preferred, else e.g. 48 kHz with
 * on-board resampling — some dongles are 48 k-only).
 *
 * FULL-DUPLEX (USB_HOST_FULL_DUPLEX): some headsets — notably the Apple USB-C
 * EarPods (a "Headset" terminal, not "Headphones") — only route USB audio to
 * their speaker while a bidirectional "call" is active. Their playback config
 * (UAC 2.0) has a fixed mixer (bmMixerControls=0) whose USB->speaker crosspoint
 * is connected only in that state. So in addition to the speaker OUT stream we
 * also claim the mic capture interface and submit iso IN transfers (data
 * discarded) to keep the capture stream live and un-gate the speaker. This is
 * best-effort and inert on output-only DACs (which expose no iso IN audio EP).
 */

#include "audio_output.h"

#include "audio_receiver.h"
#include "audio_resample.h"
#include "led.h"
#include "playback_control.h" /* play/pause + volume via DACP */
#include "rtsp_server.h"      /* airplay_get_volume_q15() */
#include "settings.h"         /* device volume persistence */
#include "spiram_task.h"      /* task_create_spiram() */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAG "audio_uac_host"
/* PREFERRED output rate (44.1 kHz = native AirPlay, no resampling). A device
 * that doesn't list it in its descriptors runs at the closest rate it does
 * support (s_out_rate) with on-board resampling — e.g. the Sony INZONE Buds
 * dongle is 48 k-only: it ACKs SET_CUR(44100) on the EP but plays silence. */
#define OUTPUT_RATE   CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_SAMPLES 352
/* Sized for the HIGHEST rate a device can force (48 k), not OUTPUT_RATE —
 * s_out_rate is chosen per device at runtime. */
#define MAX_RESAMPLE_FRAMES \
  ((size_t)((FRAME_SAMPLES + 2) * (48000.0 / 44100) + 16))

#if CONFIG_FREERTOS_UNICORE
#define PLAYBACK_CORE 0
#else
#define PLAYBACK_CORE 1
#endif
#define USB_CORE     0
#define USB_LIB_PRIO 9
/* The client task dispatches iso-OUT completions and RESUBMITS the URBs.
 * The iso schedule has hard sub-millisecond service needs: if resubmission
 * is starved longer than the in-flight ring (NUM_URBS x PACKETS_PER_URB =
 * 32 ms), the schedule idles frames and the OUTPUT RATE SILENTLY DROPS —
 * measured ~9% slow (1250 8-ms URBs taking 10.9 s) with this task at prio
 * 6 below the core-0 RTP receive task (prio 8), whose WiFi-clump decode
 * bursts run 30-80 ms. The receive task has a 512 ms mailbox of slack; the
 * iso schedule has none — so the client task must outrank it. */
#define USB_CLI_PRIO 9
/* Playback feeds the iso-OUT FIFO on a hard ~8 ms deadline while the realtime
 * RTP receive/decode task (prio 8, shared code) has seconds of jitter buffer
 * to spare — so playback must outrank it. At the old prio 7 the decode task
 * preempted playback during burst decodes and starved the FIFO: underruns
 * accumulated (~1 / 2.5 s) even at ZERO packet loss. Fixing it HERE (raise the
 * consumer) instead of lowering the shared receive task keeps the change in
 * this backend and leaves other outputs' scheduling untouched. */
#define PLAYBACK_PRIO 10

/* Largest iso-OUT wMaxPacketSize the controller can take. On S2/S3 the
 * periodic TX FIFO is carved to 150 lines = 600 B in audio_output_init(); an
 * alt-setting with a bigger EP (e.g. 96k/32-bit stereo = 768 B) can never get
 * a pipe ("EP MPS exceeds supported limit"), so skip it at selection time. */
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
#define ISO_OUT_MPS_LIMIT 600
#else
/* HS targets (P4): balanced-bias periodic OUT */
#define ISO_OUT_MPS_LIMIT 512
#endif

/* Isochronous OUT transfer pool. Full-speed: 1 packet per 1 ms frame.
 * 48 kHz 16-bit stereo = 192 bytes/frame. */
#define NUM_URBS        4
#define PACKETS_PER_URB 8 /* 8 ms per URB; 32 ms of transfers in flight */
#define NUM_IN_URBS     2 /* mic-IN capture pool (full-duplex un-gate) */

/* Open the mic-IN stream together with the speaker-OUT stream. Set to 0 to
 * stream output only (plain USB DAC, or to A/B-test the un-gate effect). */
/* Full-duplex (open the mic IN stream alongside the speaker) was an attempt to
 * "un-gate" headsets — disproven: the silence was the missing SET_INTERFACE,
 * not gating. Playback-only is correct; leave at 0. */
#define USB_HOST_FULL_DUPLEX 0

/* PCM FIFO (producer = playback task, consumer = iso OUT callbacks) */
#define FIFO_CAP 16384 /* bytes; ~85 ms at 48 kHz stereo 16-bit */
/* Target FIFO depth (~80 ms). The playback task fills to here then yields, so
 * it paces itself to the iso drain rate instead of busy-spinning. The old loop
 * never delayed while frames were available — it pegged this core at 100% and
 * jittered the shared timing layer into dropping frames as "late" (the
 * play-stop-play-stop stutter). Raised 50 -> 80 ms (2026-07-09): the timing
 * layer occasionally withholds frames for tens of ms (early-hold, refill
 * bursts) and 50 ms of headroom let the FIFO hit empty = audible gap; the
 * extra 30 ms of pipeline latency is absorbed by the sender lead (buffered)
 * and the 250 ms timing threshold (realtime).
 * Raised 80 -> 160 ms nominal (2026-08-31, = ~147 ms at a 48 k sink): with
 * the RTP receive task pinned to core 0 (see audio_stream_realtime.c), WiFi
 * delivery clumps stall frame delivery long enough to dent an 80 ms FIFO
 * (~1 audible partial pop per 2.5 s); at >=147 ms the underruns stop
 * (verified: gap=0, underruns frozen over multi-minute 48 k runs). The
 * depth is self-reported per-rate through
 * audio_output_get_hardware_latency_us(), so A/V sync is unaffected.
 * v1.1: the stream buffer is ALLOCATED for the largest selectable format
 * (96 kHz / 24-bit stereo = 576 B/ms x 160 ms), while the FILL level is
 * capped per-rate at s_fifo_target (~160 ms at the negotiated rate), so a
 * 48 kHz session holds 160 ms of audio, not 320 ms. */
#define FIFO_TARGET_BYTES ((96U * 6U) * 160U)

/* ── USB / streaming state (owned by the USB client task) ────────────────── */
static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_dev = NULL;
static volatile bool s_streaming = false;
/* Resampler rebuild request. Only the playback task may call
 * audio_resample_init/reset once it is running — see playback_task(). */
static volatile bool resample_reinit_needed = false;

static uint8_t s_out_iface = 0;
static uint8_t s_out_alt = 0;
static uint8_t s_out_ep = 0;
static uint16_t s_out_mps = 0;
/* Fractional iso packet sizing so OUTPUT_RATE can match the source (44.1 kHz)
 * with NO resampling — packets carry 44 samples most 1 ms frames, 45 every
 * ~10th, averaging 44.1k. */
static int s_frame_bytes = 4;  /* bytes per audio frame (16-bit stereo) */
static int s_pkt_base = 0;     /* floor samples/frame = OUTPUT_RATE/1000 */
static int s_pkt_frac = 0;     /* OUTPUT_RATE % 1000 */
static int s_frac_accum = 0;   /* fractional-sample accumulator */
static uint8_t s_ac_iface = 0; /* AudioControl interface (for class requests) */
static uint8_t s_clock_id = 0; /* UAC2 Clock Source entity ID (0 = none/UAC1) */
static uint8_t s_fu_ids[8];    /* Feature Unit IDs found in the AC interface */
static uint8_t s_fu_src[8];    /* each FU's bSourceID (upstream entity) */
static int s_fu_count = 0;
static uint8_t s_speaker_src =
    0;                         /* entity feeding the speaker output terminal */
static uint8_t s_usb_it = 0;   /* USB-streaming INPUT terminal (host audio) */

/* Per-FU table for the web UI: some cards (KEF EGG) have several FUs and the
 * one in the audible path isn't always the one topology matching picks. */
static fu_info_t s_fu_info[8];
static int s_fu_info_n = 0;

/* ── Device-volume state (web slider = the card's own FU volume) ─────────── */
static float s_device_db = 0.0f;    /* -30..max dB, web slider (persisted) */
static int32_t s_device_q15 = 32768; /* software device gain (Q15) */
static uint8_t s_spk_fu = 0;         /* primary speaker-path FU id (0 = none) */
static volatile float s_fu_vol_db = 0.0f; /* live FU volume readback (dB) */
static float s_fu_ch1_db = 0.0f, s_fu_ch2_db = 0.0f; /* per-channel volumes */
static volatile uint32_t s_rate_readback = 0; /* EP rate GET_CUR (0 = unread) */
static uint8_t s_cfg_raw[512];                 /* raw config descriptor copy */
static uint16_t s_cfg_raw_n = 0;
static float s_fu_min_db = 0.0f, s_fu_max_db = 0.0f; /* FU volume range */
static fu_volume_probe_t s_fu_probe;  /* last FU probe results (web display) */
static uint8_t s_mixer_id = 0; /* Mixer Unit ID (0 = none) */
static uint8_t s_mic_fu = 0;   /* feature unit feeding the USB-out (mic) term */
static uint8_t s_clock_ids[4]; /* all UAC2 Clock Source entity IDs */
static int s_clock_count = 0;
static int s_out_subslot =
    2; /* device sample size (bytes): 2=16-bit, 3=24-bit */
static uint32_t s_out_rate =
    OUTPUT_RATE;              /* rate the chosen alt actually runs */
static uint8_t s_uac_ver = 0; /* UAC spec major version: 1 or 2 */

/* ── User-chosen output format (v1.1) ───────────────────────────────────────
 * The web UI picks one of six rate x bits combinations. Resolved once per
 * setup_device() from settings, it drives the alt/rate preference below
 * (OUTPUT_RATE / 24-bit remain only as the compile-time fallback). */
static uint32_t s_chosen_rate = OUTPUT_RATE;
static uint8_t s_chosen_subslot = 3; /* 24-bit, matches Windows default */

static void resolve_chosen_format(void) {
  uint8_t fmt = settings_get_audio_fmt();
  settings_audio_fmt_params(fmt, &s_chosen_rate, NULL, &s_chosen_subslot);
  ESP_LOGI(TAG, "User format: %s (rate=%lu subslot=%u)", settings_audio_fmt_label(fmt),
           (unsigned long)s_chosen_rate, (unsigned)s_chosen_subslot);
}

/* All iso PCM altsettings found in the config descriptor — the web UI's
 * "可用格式" menu (which formats the attached card actually offers). */
#define MAX_ALT_INFO 12
static usb_alt_info_t s_alt_info[MAX_ALT_INFO];
static int s_alt_count = 0;

/* Where the last device-attach attempt stopped, and the esp_err of its
 * failure — surfaced in the web UI so a broken enumeration can be diagnosed
 * without a serial port (the OTG port occupies the only debug UART). */
static volatile int s_setup_stage = 0;
static volatile int s_setup_err = 0;

/* USB device iProduct string — shown in the web UI "音频编码状态" module so the
 * user can see which sound card negotiated what encoding. */
static char s_product[64] = "";

/* Full-duplex capture path (mic IN). Opening it alongside the speaker OUT can
 * un-gate a headset that only routes USB audio during an active bidirectional
 * "call" (e.g. Apple EarPods). Best-effort; absent on output-only DACs. */
static uint8_t s_in_iface = 0xff;
static uint8_t s_in_alt = 0;
static uint8_t s_in_ep = 0;
static uint16_t s_in_mps = 0;
static int s_in_channels = 0;
static int s_in_packet_bytes = 0;
static usb_transfer_t *s_in_urb[NUM_IN_URBS];
static volatile bool s_capturing = false;
static volatile uint32_t s_in_done = 0, s_in_err = 0;

/* HID media-button path: the headset's own play/volume keys arrive as HID
 * interrupt reports; we forward them to the AirPlay source via DACP. */
static uint8_t s_hid_iface = 0xff; /* 0xff = device has no HID interface */
static uint8_t s_hid_alt = 0;
static uint8_t s_hid_ep = 0;
static uint16_t s_hid_mps = 0;
static usb_transfer_t *s_hid_urb = NULL;
static volatile bool s_hid_active = false;
static volatile uint32_t s_hid_events = 0; /* remote/HID key presses seen */
static volatile uint32_t s_hid_volup = 0, s_hid_voldown = 0;
static volatile uint8_t s_hid_last_raw[4] = {0};
static volatile uint8_t s_hid_last_n = 0;
static volatile uint8_t s_hid_press_raw[4] = {0}; /* raw bytes at a PRESS edge */
static volatile uint8_t s_hid_press_n = 0;
static uint8_t s_hid_prev = 0;              /* previous bitmap (edge detect) */
static uint8_t s_hid_prev_b2 = 0;           /* previous byte[2] (edge detect) */
static QueueHandle_t s_hid_action_q = NULL; /* HID button -> action task */
/* Remote HID button bitmaps. Two mutually exclusive vendor layouts use the
 * same byte[1] bit values with opposite meanings, so the active one is a
 * user setting (settings_remote_layout_egg):
 *   Apple standard (default, byte[1]):  play/pause=0x01, vol+=0x02, vol-=0x04
 *   KEF EGG (byte[1]): vol+=0x01, vol-=0x02; byte[2]: play=0x20, next=0x40,
 *   prev=0x80. byte[3] = 0x80 | active (both).
 * byte[2] transport bits are additive — Apple headsets leave byte[2] zero. */
#define HID_AP_PLAY_PAUSE 0x01
#define HID_AP_VOL_UP     0x02
#define HID_AP_VOL_DOWN   0x04
#define HID_EGG_VOL_UP    0x01
#define HID_EGG_VOL_DOWN  0x02
#define HID_B2_PLAY_PAUSE 0x20
#define HID_B2_NEXT       0x40
#define HID_B2_PREV       0x80

static usb_transfer_t *s_urb[NUM_URBS];
static SemaphoreHandle_t s_ctrl_sem =
    NULL; /* signals sync control completion */

/* connect/disconnect handshake from the client callback to the client task */
static volatile bool s_connect_pending = false;
static volatile bool s_disconnect_pending = false;
static volatile uint8_t s_pending_addr = 0;
static uint8_t s_dev_addr = 0; /* USB address of the attached card (reprobe) */

/* ── PCM queue ─────────────────────────────────────────────────────────────
 * A FreeRTOS stream buffer gives event-driven backpressure: the playback task
 * blocks in xStreamBufferSend until the iso-OUT callbacks drain space, so it
 * paces itself exactly to the device's consumption rate — no busy-poll, no
 * drop-on-full. Sized to FIFO_TARGET_BYTES (~50 ms): it runs near full, keeping
 * the output buffering stable at that depth (see hardware_latency_us). */
static StreamBufferHandle_t s_pcm = NULL;

/* telemetry */
static volatile uint32_t s_xfer_done = 0, s_xfer_err = 0, s_pkt_err = 0;
static volatile uint32_t s_push_bytes = 0;

/* Set to make the playback task reset + silence-prefill the FIFO at the top
 * of its loop. The playback task is the stream buffer's only writer and the
 * only caller of fifo_reset(); other tasks (connect/teardown, AirPlay flush)
 * request the reset through this flag instead of touching the FIFO. */
static volatile bool flush_requested = false;

/* Per-rate FIFO fill target (~160 ms at the negotiated rate x subslot).
 * The stream buffer itself is allocated at FIFO_TARGET_BYTES (the largest
 * format); this cap keeps the playout depth at ~160 ms on slower formats. */
static size_t s_fifo_target = FIFO_TARGET_BYTES;

/* fifo_reset() <-> iso-callback handshake: while s_fifo_hold is set the
 * callback emits silence without touching the stream buffer, and
 * s_iso_cb_gen ticks at the END of every callback invocation. */
static volatile bool s_fifo_hold = false;
static volatile uint32_t s_iso_cb_gen = 0;

static int fifo_level(void) {
  return s_pcm ? (int)xStreamBufferBytesAvailable(s_pcm) : 0;
}

/* Reset without racing the reader: xStreamBufferReset() must never run
 * concurrently with the callback's xStreamBufferReceive() — the reader-side
 * tail update is lock-free, so a reset under it corrupts the buffer
 * accounting. Raise the hold, then wait for one callback generation tick:
 * callbacks are serialized on the USB client task, so a tick means any
 * receive that was in flight when the hold went up has finished. The
 * timeout covers a stopped stream (callbacks no longer ticking). Called
 * from the playback task only — it is the sole writer, so no send can be
 * in flight either. */
static void fifo_reset(void) {
  if (!s_pcm)
    return;
  s_fifo_hold = true;
  if (s_streaming) {
    uint32_t g0 = s_iso_cb_gen;
    for (int i = 0; i < 50 && s_iso_cb_gen == g0; i++)
      vTaskDelay(1);
  }
  xStreamBufferReset(s_pcm);
  s_fifo_hold = false;
}

/* Refill the FIFO with silence after a reset. Starting from an EMPTY FIFO,
 * the playback task races ~112 ms of REAL audio (FIFO + URB ring) into the
 * pipeline at once, permanently parking playout that far ahead of schedule —
 * right at the early-hold boundary, where anchor wobble quantizes into
 * audible clicks. Pre-filled with silence, the producer is pipeline-paced
 * from its very first real frame and playout starts on schedule. */
static void fifo_prefill_silence(void) {
  if (!s_pcm)
    return;
  static const uint8_t zeros[512] = {0};
  size_t space = s_fifo_target; /* prefill to the per-rate target, not capacity */
  while (space >= sizeof(zeros)) {
    xStreamBufferSend(s_pcm, zeros, sizeof(zeros), 0);
    space -= sizeof(zeros);
  }
  if (space)
    xStreamBufferSend(s_pcm, zeros, space, 0);
}

/* Enqueue, blocking (paced by the consumer) until there is room. A long stall
 * (e.g. the device was unplugged) eventually drops the remainder rather than
 * wedging the playback task — but NEVER at an arbitrary byte offset: the FIFO
 * is a raw byte stream, so losing a non-multiple of the frame size would
 * shift every later frame boundary (channel swap + byte-shift = loud static
 * until the next flush). On timeout, retry; if the FIFO stays full, drop the
 * tail but zero-pad back to a frame boundary. */
static void fifo_push(const uint8_t *src, size_t len) {
  if (!s_pcm)
    return;
  s_push_bytes += len;
  size_t sent = 0;
  for (int tries = 0; sent < len && tries < 8 && s_streaming; tries++) {
    /* Cap the fill level at the per-rate target: the buffer is allocated for
     * the largest format, and letting it fill to capacity at a slower rate
     * would more than double the playout depth. Sleep instead of a blocking
     * send when the target is reached — the iso drain wakes this loop. */
    size_t level = fifo_level();
    if (level >= s_fifo_target) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    size_t chunk = len - sent;
    size_t room = s_fifo_target - level;
    if (chunk > room)
      chunk = room;
    sent +=
        xStreamBufferSend(s_pcm, src + sent, chunk, pdMS_TO_TICKS(100));
  }
  if (sent < len) {
    size_t mis = sent % (size_t)s_frame_bytes;
    if (mis) {
      static const uint8_t pad[8] = {0};
      xStreamBufferSend(s_pcm, pad, (size_t)s_frame_bytes - mis,
                        pdMS_TO_TICKS(20));
    }
  }
}

/* Dequeue up to `len` bytes (non-blocking — runs in the USB callback); pad the
 * remainder with silence on underrun. */
static uint32_t s_underruns = 0; /* mid-stream FIFO underruns (audible gaps) */

static void fifo_pop_padded(uint8_t *dst, size_t len) {
  if (s_fifo_hold || !s_pcm) {
    memset(dst, 0, len); /* fifo_reset() in progress — stay off the buffer */
    s_iso_cb_gen++;
    return;
  }
  size_t got = xStreamBufferReceive(s_pcm, dst, len, 0);
  if (got < len) {
    memset(dst + got, 0, len - got);
    /* A PARTIAL pop is the moment an active stream ran dry — an audible
     * gap. (Full-zero pops also happen while idle, so don't count those.) */
    if (got > 0) {
      s_underruns++;
      static int64_t s_last_ur_log = 0;
      int64_t now = esp_timer_get_time();
      if (now - s_last_ur_log > 2000000) {
        s_last_ur_log = now;
        ESP_LOGW(TAG, "FIFO underrun #%lu: gap at USB boundary",
                 (unsigned long)s_underruns);
      }
    }
  }
  s_iso_cb_gen++; /* tick only after all buffer access is complete */
}

/* ── Optional VBUS enable ────────────────────────────────────────────────── */
static void usb_host_vbus_enable(void) {
#if defined(CONFIG_USB_HOST_VBUS_EN_GPIO) && CONFIG_USB_HOST_VBUS_EN_GPIO >= 0
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_USB_HOST_VBUS_EN_GPIO,
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io);
  gpio_set_level(CONFIG_USB_HOST_VBUS_EN_GPIO, 1);
  ESP_LOGI(TAG, "VBUS enable GPIO%d -> HIGH", CONFIG_USB_HOST_VBUS_EN_GPIO);
#endif
}

/* ── USB host library daemon ─────────────────────────────────────────────── */
static void usb_lib_task(void *arg) {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
      usb_host_device_free_all();
  }
}

/* ── Descriptor parsing ──────────────────────────────────────────────────── */
/* Walk the (active) config descriptor and find an audio STREAMING interface
 * alt-setting that is PCM 16-bit / 2-channel on an isochronous OUT endpoint.
 * Records iface/alt/endpoint/MPS. Returns true on success. */
static bool find_speaker_altsetting(const usb_config_desc_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg;
  const uint8_t *end = p + cfg->wTotalLength;
  p += p[0]; /* skip the 9-byte config descriptor */

  int iface = -1, alt = -1;
  bool cur_is_as = false;  /* current alt is an AudioStreaming interface */
  bool cur_is_ac = false;  /* current interface is AudioControl */
  bool cur_is_hid = false; /* current interface is HID (media buttons) */
  int as_channels = 0, as_bits = 0, as_subslot = 0;
  uint32_t as_rate = 0; /* rate this alt would run at (0 = unknown, UAC2) */
  uint32_t as_rates[3] = {0, 0, 0}; /* every rate this alt lists (UAC1) */
  int as_rates_n = 0;
  bool found_speaker = false;
  int best_rank = -1; /* rank of the speaker alt held so far (see below) */
  s_ac_iface = 0;
  s_uac_ver = 0;
  s_out_subslot = 2;
  s_out_rate = OUTPUT_RATE;
  s_clock_id = 0;
  s_clock_count = 0;
  s_fu_count = 0;
  s_speaker_src = 0;
  s_usb_it = 0;
  s_mic_fu = 0;
  s_mixer_id = 0;
  s_alt_count = 0;
  s_in_iface = 0xff;
  s_in_alt = 0;
  s_in_ep = 0;
  s_in_mps = 0;
  s_in_channels = 0;
  s_hid_iface = 0xff;
  s_hid_ep = 0;
  s_hid_mps = 0;

  while (p + 2 <= end) {
    uint8_t len = p[0], type = p[1];
    if (len < 2 || p + len > end)
      break;

    if (type == 0x04) { /* INTERFACE */
      iface = p[2];
      alt = p[3];
      uint8_t cls = p[5], sub = p[6], proto = p[7];
      cur_is_ac = (cls == 0x01 && sub == 0x01); /* AUDIO / AUDIOCONTROL */
      cur_is_as = (cls == 0x01 && sub == 0x02); /* AUDIO / AUDIOSTREAMING */
      cur_is_hid = (cls == 0x03);               /* HID (media buttons) */
      if (cur_is_ac) {
        s_ac_iface = (uint8_t)iface;
        s_uac_ver =
            (proto == 0x20) ? 2 : 1; /* bInterfaceProtocol 0x20 = UAC2 */
      }
      if (cur_is_hid && s_hid_iface == 0xff) {
        s_hid_iface = (uint8_t)iface;
        s_hid_alt = (uint8_t)alt;
      }
      as_channels = 0;
      as_bits = 0;
      as_subslot = 0;
      as_rate = 0;
      as_rates_n = 0;
    } else if (cur_is_ac && type == 0x24) { /* CS_INTERFACE (AudioControl) */
      /* INPUT_TERMINAL (subtype 2): grab the clock feeding the USB-streaming
       * (host->device) terminal. UAC2 layout: wTerminalType @4, bCSourceID @7
       */
      if (p[2] == 0x02 && len >= 8) {
        /* INPUT_TERMINAL: clock of the USB-streaming (host->device) terminal */
        uint16_t ttype = (uint16_t)(p[4] | (p[5] << 8));
        if (ttype == 0x0101) { /* USB Streaming */
          s_usb_it = p[3];     /* terminal ID — anchors the speaker path */
          s_clock_id = p[7];
        }
      } else if (p[2] == 0x03 && len >= 8) {
        /* OUTPUT_TERMINAL: bSourceID @7 feeds this terminal. A non-USB output
         * (speaker/headset) is the physical speaker; a USB-streaming output is
         * the mic->host path, fed by the mic feature unit. */
        uint16_t ttype = (uint16_t)(p[4] | (p[5] << 8));
        if ((ttype >> 8) != 0x01)
          s_speaker_src = p[7];
        else
          s_mic_fu = p[7];
      } else if (p[2] == 0x04 && len >= 5) {
        /* MIXER_UNIT: bUnitID @3 */
        s_mixer_id = p[3];
      } else if (p[2] == 0x06 && len >= 5 && s_fu_count < 8) {
        /* FEATURE_UNIT: bUnitID @3, bSourceID @4 (both UAC1 and UAC2) */
        s_fu_src[s_fu_count] = p[4];
        s_fu_ids[s_fu_count++] = p[3];
      } else if (p[2] == 0x0a && len >= 4 &&
                 s_clock_count < (int)sizeof(s_clock_ids)) {
        /* CLOCK_SOURCE: bClockID @3 — collect so we set every clock's rate */
        s_clock_ids[s_clock_count++] = p[3];
      }
    } else if (cur_is_as && type == 0x24) { /* CS_INTERFACE (AudioStreaming) */
      uint8_t subtype = p[2];
      if (subtype == 0x01) {
        /* AS_GENERAL: UAC2 carries bNrChannels @10 (UAC1 carries it in the
         * FORMAT_TYPE descriptor below). */
        if (s_uac_ver == 2 && len >= 11)
          as_channels = p[10];
      } else if (subtype == 0x02) {
        /* FORMAT_TYPE_I: UAC2 → bSubslotSize@4, bBitResolution@5;
         *                UAC1 → bNrChannels@4, bSubframeSize@5,
         * bBitResolution@6 */
        if (s_uac_ver == 2) {
          if (len >= 6) {
            as_subslot = p[4];
            as_bits = p[5];
          }
        } else if (len >= 7) {
          as_channels = p[4];
          as_subslot = p[5];
          as_bits = p[6];
          /* UAC1 lists the supported rates right here: bSamFreqType @7
           * (0 = continuous min..max, else count of discrete rates), 3-byte
           * rates from @8. Pick per alt: 44.1 k native > 48 k resampled >
           * first listed. Do NOT assume a device accepts OUTPUT_RATE just
           * because SET_CUR succeeds — 48 k-only dongles (Sony INZONE Buds)
           * ACK 44100 and then play silence. */
          if (len >= 8) {
            uint8_t nfreq = p[7];
            if (nfreq == 0 && len >= 14) { /* continuous range */
              uint32_t lo = p[8] | (p[9] << 8) | ((uint32_t)p[10] << 16);
              uint32_t hi = p[11] | (p[12] << 8) | ((uint32_t)p[13] << 16);
              as_rate = (s_chosen_rate >= lo && s_chosen_rate <= hi)
                            ? s_chosen_rate
                            : (44100 >= lo && 44100 <= hi) ? 44100
                            : (48000 >= lo && 48000 <= hi) ? 48000 : lo;
              as_rates[0] = lo;
              as_rates[1] = hi;
              as_rates_n = 2; /* range hint for the web menu */
            } else {
              as_rates_n = 0;
              for (int f = 0; f < nfreq && 8 + 3 * f + 2 < len &&
                          as_rates_n < 3; f++) {
                uint32_t r = p[8 + 3 * f] | (p[9 + 3 * f] << 8) |
                             ((uint32_t)p[10 + 3 * f] << 16);
                as_rates[as_rates_n++] = r;
                /* Prefer the USER-CHOSEN rate when the device lists it.
                 * A UAC1 device (e.g. KEF EGG) may list 44.1/48/96 k in ONE
                 * alt; always picking 44.1 k kept it on the quiet/glitchy
                 * path. First listed remains the fallback. Do NOT assume a
                 * device accepts the chosen rate just because SET_CUR
                 * succeeds — 48 k-only dongles (Sony INZONE Buds) ACK 44100,
                 * so a rate a device never lists is never forced. */
                if (r == s_chosen_rate)
                  as_rate = r;
              }
              if (as_rate == 0)
                as_rate = as_rates[0]; /* fallback: first listed */
            }
          }
        }
      }
    } else if (cur_is_as && type == 0x05) { /* ENDPOINT */
      uint8_t ep_addr = p[2], ep_attr = p[3];
      uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
      bool is_out = !(ep_addr & 0x80);
      bool is_iso = (ep_attr & 0x03) == 0x01;
      /* Record every iso PCM alt in the menu (web "可用格式" list) */
      if (is_iso && mps > 0 && as_subslot >= 2 && as_subslot <= 4 &&
          s_alt_count < MAX_ALT_INFO) {
        s_alt_info[s_alt_count].rate = as_rate ? as_rate : OUTPUT_RATE;
        s_alt_info[s_alt_count].bits = (uint8_t)(as_subslot * 8);
        s_alt_info[s_alt_count].subslot = (uint8_t)as_subslot;
        s_alt_info[s_alt_count].mps = mps;
        s_alt_info[s_alt_count].sync = (uint8_t)((ep_attr >> 2) & 0x03);
        s_alt_info[s_alt_count].out = is_out ? 1 : 0;
        s_alt_info[s_alt_count].channels = (uint8_t)as_channels;
        s_alt_info[s_alt_count].iface = (uint8_t)iface;
        s_alt_info[s_alt_count].alt = (uint8_t)alt;
        s_alt_info[s_alt_count].rates_n =
            (uint8_t)(as_rates_n > 0 ? as_rates_n : 1);
        s_alt_info[s_alt_count].rates[0] =
            as_rates_n > 0 ? as_rates[0] : OUTPUT_RATE;
        for (int ri = 1; ri < 3; ri++)
          s_alt_info[s_alt_count].rates[ri] = as_rates[ri];
        s_alt_count++;
      }
      /* sync type (bmAttributes[3:2]): 0=none 1=async 2=adaptive 3=sync.
       * usage (bmAttributes[5:4]): 0=data 1=feedback. An async OUT has a
       * separate feedback IN endpoint we'd need to honor to avoid drift. */
      ESP_LOGI(TAG,
               "  AS iface=%d alt=%d: ch=%d bits=%d sub=%d ep=0x%02x iso=%d "
               "out=%d sync=%d use=%d mps=%u rate=%lu",
               iface, alt, as_channels, as_bits, as_subslot, ep_addr, is_iso,
               is_out, (ep_attr >> 2) & 0x03, (ep_attr >> 4) & 0x03, mps,
               (unsigned long)as_rate);
      if (is_iso && mps > 0) {
        if (is_out && as_channels == 2 && as_subslot >= 2 && as_subslot <= 4 &&
            mps > ISO_OUT_MPS_LIMIT) {
          /* Pipe allocation would fail — a later/smaller alt may still work. */
          ESP_LOGW(TAG, "  skip alt %d: iso-OUT mps %u > HW limit %d", alt, mps,
                   ISO_OUT_MPS_LIMIT);
        } else if (is_out && as_channels == 2 && as_subslot >= 2 &&
                   as_subslot <= 4) {
          /* Stereo PCM iso OUT = the speaker path. Rank, high to low: an alt
           * whose packets can carry its rate beats one that can't (an
           * undersized EP would underrun); then the USER-CHOSEN rate — a UAC1
           * device may expose 48 k and 44.1 k as separate sibling alts, and
           * the sinc resampler costs far more CPU than a subslot expand;
           * then the USER-CHOSEN bit depth (24-bit by default) — matches the
           * KEF EGG's loud Windows path (alt2, 24-bit@48k). */
          uint32_t alt_rate = as_rate ? as_rate : OUTPUT_RATE;
          unsigned need = (unsigned)(alt_rate / 1000 + (alt_rate % 1000 != 0)) *
                          2u * (unsigned)as_subslot; /* peak bytes per 1 ms */
          int rank = (mps >= need ? 4 : 0) + (alt_rate == s_chosen_rate ? 2 : 0) +
                     (as_subslot == s_chosen_subslot ? 1 : 0);
          if (rank > best_rank) {
            s_out_iface = (uint8_t)iface;
            s_out_alt = (uint8_t)alt;
            s_out_ep = ep_addr;
            s_out_mps = mps;
            s_out_subslot = as_subslot;
            s_out_rate = alt_rate;
            found_speaker = true;
            best_rank = rank;
          }
        } else if (!is_out && as_channels >= 1 && as_subslot == 2 &&
                   s_in_ep == 0) {
          /* 16-bit iso IN = the mic capture path (opened for full-duplex) */
          s_in_iface = (uint8_t)iface;
          s_in_alt = (uint8_t)alt;
          s_in_ep = ep_addr;
          s_in_mps = mps;
          s_in_channels = as_channels;
        }
      }
    } else if (cur_is_hid && type == 0x05) { /* HID interrupt endpoint */
      uint8_t ep_addr = p[2], ep_attr = p[3];
      uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
      if ((ep_addr & 0x80) && (ep_attr & 0x03) == 0x03 && s_hid_ep == 0) {
        s_hid_ep = ep_addr; /* interrupt IN — media button reports */
        s_hid_mps = mps;
        ESP_LOGI(TAG, "  HID iface=%u ep=0x%02x mps=%u (media buttons)",
                 s_hid_iface, ep_addr, mps);
      }
    }
    p += len;
  }
  return found_speaker;
}

/* ── Isochronous OUT streaming ───────────────────────────────────────────── */
/* Size each iso packet for the (possibly fractional) rate; sets per-packet
 * num_bytes + xfer->num_bytes, returns the urb total. */
static int fill_out_packets(usb_transfer_t *xfer) {
  /* Never exceed the endpoint's wMaxPacketSize: the chooser accepts an
   * undersized iso EP as a last resort, and the URB buffer is allocated
   * to MPS in that case — a packet sized from the rate alone would
   * overrun both the buffer and the device limit. Return the unsent
   * fraction to the accumulator (bounded) so a marginally-small EP still
   * averages out; a fundamentally-too-small EP degrades gracefully. */
  int max_samples = (int)(s_out_mps / (uint16_t)s_frame_bytes);
  int total = 0;
  for (int j = 0; j < PACKETS_PER_URB; j++) {
    int samples = s_pkt_base;
    s_frac_accum += s_pkt_frac;
    if (s_frac_accum >= 1000) {
      samples++;
      s_frac_accum -= 1000;
    }
    if (samples > max_samples) {
      s_frac_accum += (samples - max_samples) * 1000;
      if (s_frac_accum > 2000)
        s_frac_accum = 2000; /* cap the debt: the EP can't sustain the rate */
      samples = max_samples;
    }
    int bytes = samples * s_frame_bytes;
    xfer->isoc_packet_desc[j].num_bytes = bytes;
    total += bytes;
  }
  xfer->num_bytes = total;
  return total;
}

static void out_xfer_cb(usb_transfer_t *xfer) {
  if (!s_streaming)
    return; /* tearing down — do not resubmit */
  s_xfer_done++;
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED)
    s_xfer_err++;
  for (int j = 0; j < PACKETS_PER_URB; j++)
    if (xfer->isoc_packet_desc[j].status != USB_TRANSFER_STATUS_COMPLETED)
      s_pkt_err++;

  int total = fill_out_packets(xfer); /* fractional sizing + xfer->num_bytes */
  fifo_pop_padded(xfer->data_buffer, (size_t)total);

  if ((s_xfer_done % 1250) == 0) { /* ~ every 10 s (1250 urbs * 8 ms) */
    /* rx/gap deltas answer "did every UDP packet arrive in time": gap counts
     * RTP sequence holes at ARRIVAL (before any buffering/timing). */
    static uint32_t s_prev_rx = 0, s_prev_gap = 0;
    audio_stats_t st;
    audio_receiver_get_stats(&st);
    uint32_t rx_d = st.packets_received - s_prev_rx;
    uint32_t gap_d = st.packets_dropped - s_prev_gap;
    s_prev_rx = st.packets_received;
    s_prev_gap = st.packets_dropped;
    ESP_LOGI(TAG,
             "tlm xfers=%lu xerr=%lu pkterr=%lu push=%luB fifo=%d/%d "
             "ur=%lu rx=%lu gap=%lu",
             (unsigned long)s_xfer_done, (unsigned long)s_xfer_err,
             (unsigned long)s_pkt_err, (unsigned long)s_push_bytes,
             fifo_level(), (unsigned long)s_fifo_target,
             (unsigned long)s_underruns,
             (unsigned long)rx_d, (unsigned long)gap_d);
  }

  /* Resubmit regardless of transient status so the iso stream keeps flowing;
   * a real disconnect arrives via the DEV_GONE client event. */
  esp_err_t e = usb_host_transfer_submit(xfer);
  if (e != ESP_OK && (s_xfer_done % 1250) == 0)
    ESP_LOGW(TAG, "resubmit iso OUT: %s", esp_err_to_name(e));
}

static esp_err_t start_streaming(void) {
  /* Match the source rate (no resampling). Fractional packets: e.g. 44.1 kHz =
   * 44 samples/frame, 45 every ~10th. Allocate the worst case (one extra
   * sample/packet), capped at the endpoint MPS. */
  s_frame_bytes = 2 * s_out_subslot; /* 4 = 16-bit, 6 = 24-bit (stereo) */
  s_pkt_base = (int)(s_out_rate / 1000);
  s_pkt_frac = (int)(s_out_rate % 1000);
  s_frac_accum = 0;
  /* v1.1: the fill target follows the negotiated format (~160 ms). */
  s_fifo_target =
      (size_t)s_frame_bytes * (size_t)(s_out_rate / 1000) * 160U;
  if (s_fifo_target > FIFO_TARGET_BYTES)
    s_fifo_target = FIFO_TARGET_BYTES;
  int alloc = (s_pkt_base + 1) * s_frame_bytes * PACKETS_PER_URB;
  if (alloc > (int)s_out_mps * PACKETS_PER_URB)
    alloc = (int)s_out_mps * PACKETS_PER_URB;

  /* FIFO reset + silence prefill belong to the playback task (the buffer's
   * sole writer and fifo_reset()'s only legal caller); it services this flag
   * at the top of its loop. Until then the callback zero-pads from the FIFO
   * — silence either way, since teardown_device() flushed it. */
  flush_requested = true;
  s_streaming = true;
  /* Prime ALL URBs before submitting ANY: fill_out_packets() advances the
   * shared fractional accumulator, and the moment the first URB is
   * submitted its completion callback (USB client task) starts calling it
   * too — interleaving with this loop would race the accumulator and
   * corrupt 44.1 kHz fractional packet sizing. With the split, the
   * callback is the accumulator's only caller once submits begin. */
  for (int i = 0; i < NUM_URBS; i++) {
    ESP_RETURN_ON_ERROR(
        usb_host_transfer_alloc(alloc, PACKETS_PER_URB, &s_urb[i]), TAG,
        "transfer_alloc");
    s_urb[i]->device_handle = s_dev;
    s_urb[i]->bEndpointAddress = s_out_ep;
    s_urb[i]->callback = out_xfer_cb;
    s_urb[i]->context = NULL;
    memset(s_urb[i]->data_buffer, 0, alloc); /* prime with silence */
    fill_out_packets(s_urb[i]);              /* sets packet sizes + num_bytes */
  }
  for (int i = 0; i < NUM_URBS; i++)
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_urb[i]), TAG, "submit");
  ESP_LOGI(
      TAG,
      "Streaming: ep=0x%02x rate=%lu (%d+frac samp/frame) x %d pkt x %d urbs",
      s_out_ep, (unsigned long)s_out_rate, s_pkt_base, PACKETS_PER_URB,
      NUM_URBS);
  return ESP_OK;
}

/* ── Isochronous IN capture (full-duplex) ────────────────────────────────── */
/* We don't use the mic audio; resubmitting IN transfers just keeps the capture
 * stream live so a headset treats this as an active call and routes USB->spkr.
 */
static void in_xfer_cb(usb_transfer_t *xfer) {
  if (!s_capturing)
    return; /* tearing down — do not resubmit */
  s_in_done++;
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED)
    s_in_err++;
  for (int j = 0; j < PACKETS_PER_URB; j++)
    xfer->isoc_packet_desc[j].num_bytes = s_in_packet_bytes; /* discard data */
  xfer->num_bytes = s_in_packet_bytes * PACKETS_PER_URB;
  if ((s_in_done % 500) == 0)
    ESP_LOGI(TAG, "tlm mic-IN done=%lu err=%lu", (unsigned long)s_in_done,
             (unsigned long)s_in_err);
  usb_host_transfer_submit(xfer);
}

static esp_err_t start_capture(void) {
  s_in_packet_bytes = s_in_channels * 2 * (int)(s_out_rate / 1000);
  if (s_in_packet_bytes > s_in_mps)
    s_in_packet_bytes = s_in_mps;
  int total = s_in_packet_bytes * PACKETS_PER_URB;
  s_capturing = true;
  for (int i = 0; i < NUM_IN_URBS; i++) {
    if (usb_host_transfer_alloc(total, PACKETS_PER_URB, &s_in_urb[i]) !=
        ESP_OK) {
      s_in_urb[i] = NULL;
      goto fail;
    }
    s_in_urb[i]->device_handle = s_dev;
    s_in_urb[i]->bEndpointAddress = s_in_ep;
    s_in_urb[i]->callback = in_xfer_cb;
    s_in_urb[i]->context = NULL;
    s_in_urb[i]->num_bytes = total;
    for (int j = 0; j < PACKETS_PER_URB; j++)
      s_in_urb[i]->isoc_packet_desc[j].num_bytes = s_in_packet_bytes;
    if (usb_host_transfer_submit(s_in_urb[i]) != ESP_OK)
      goto fail;
  }
  return ESP_OK;
fail:
  s_capturing = false;
  for (int i = 0; i < NUM_IN_URBS; i++)
    if (s_in_urb[i]) {
      usb_host_transfer_free(s_in_urb[i]);
      s_in_urb[i] = NULL;
    }
  return ESP_FAIL;
}

/* Control-transfer completion handshake. The waiter and the completion
 * callback race at timeout: freeing an in-flight EP0 URB corrupts the USB
 * host stack (observed: assert in hcd_urb_dequeue at a track-change
 * teardown, hours into a session). Ownership is decided by one atomic CAS:
 * whoever loses the race cleans up. */
typedef enum {
  CTRL_WAITING = 0,
  CTRL_DONE,     /* callback fired first: waiter frees */
  CTRL_ABANDONED /* waiter timed out first: callback frees */
} ctrl_state_t;

static void ctrl_done_cb(usb_transfer_t *t) {
  int expected = CTRL_WAITING;
  if (__atomic_compare_exchange_n((int *)&t->context, &expected, CTRL_DONE,
                                  false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    xSemaphoreGive(s_ctrl_sem);
  } else {
    /* Waiter abandoned this transfer on timeout — it is ours to free. */
    usb_host_transfer_free(t);
  }
}

/* Blocking control transfer on EP0. MUST be called from a task other than the
 * USB client task (that task dispatches this transfer's completion callback).
 */
static esp_err_t ctrl_xfer_sync(uint8_t reqtype, uint8_t req, uint16_t val,
                                uint16_t idx, const uint8_t *data,
                                uint16_t len) {
  usb_transfer_t *x = NULL;
  uint16_t buflen = len ? len : 1;
  if (usb_host_transfer_alloc(8 + buflen, 0, &x) != ESP_OK)
    return ESP_ERR_NO_MEM;
  usb_setup_packet_t *s = (usb_setup_packet_t *)x->data_buffer;
  s->bmRequestType = reqtype;
  s->bRequest = req;
  s->wValue = val;
  s->wIndex = idx;
  s->wLength = len;
  if (data && len)
    memcpy(x->data_buffer + 8, data, len);
  x->num_bytes = 8 + len;
  x->device_handle = s_dev;
  x->bEndpointAddress = 0;
  x->callback = ctrl_done_cb;
  x->context = (void *)CTRL_WAITING;
  esp_err_t e = usb_host_transfer_submit_control(s_client, x);
  if (e != ESP_OK) {
    usb_host_transfer_free(x); /* never submitted — safe to free */
    return e;
  }
  if (xSemaphoreTake(s_ctrl_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
    e = (x->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
    usb_host_transfer_free(x);
    return e;
  }
  /* Timeout. Try to abandon the transfer to the callback; if the callback
   * completed in this instant, consume its semaphore give and free here. */
  int expected = CTRL_WAITING;
  if (__atomic_compare_exchange_n((int *)&x->context, &expected, CTRL_ABANDONED,
                                  false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    return ESP_ERR_TIMEOUT; /* callback will free the in-flight transfer */
  }
  xSemaphoreTake(s_ctrl_sem, 0); /* balance the give from the callback */
  e = (x->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
  usb_host_transfer_free(x);
  return e;
}

/* IN (device->host) variant of ctrl_xfer_sync: reads up to `len` bytes of a
 * class request into `out`; *out_len gets the count actually received (may
 * be short — devices truncate RANGE replies to what they have). */
static esp_err_t ctrl_xfer_sync_in(uint8_t reqtype, uint8_t req, uint16_t val,
                                   uint16_t idx, uint8_t *out, uint16_t len,
                                   int *out_len) {
  if (out_len)
    *out_len = 0;
  usb_transfer_t *x = NULL;
  if (usb_host_transfer_alloc(8 + len, 0, &x) != ESP_OK)
    return ESP_ERR_NO_MEM;
  usb_setup_packet_t *s = (usb_setup_packet_t *)x->data_buffer;
  s->bmRequestType = reqtype; /* 0xA1 = IN | class | interface */
  s->bRequest = req;
  s->wValue = val;
  s->wIndex = idx;
  s->wLength = len;
  x->num_bytes = 8 + len;
  x->device_handle = s_dev;
  x->bEndpointAddress = 0;
  x->callback = ctrl_done_cb;
  x->context = (void *)CTRL_WAITING;
  esp_err_t e = usb_host_transfer_submit_control(s_client, x);
  if (e != ESP_OK) {
    usb_host_transfer_free(x); /* never submitted — safe to free */
    return e;
  }
  if (xSemaphoreTake(s_ctrl_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
    int expected = CTRL_WAITING;
    if (__atomic_compare_exchange_n((int *)&x->context, &expected,
                                    CTRL_ABANDONED, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
      return ESP_ERR_TIMEOUT; /* callback will free the in-flight transfer */
    }
    xSemaphoreTake(s_ctrl_sem, 0); /* balance the give from the callback */
  }
  e = (x->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
  if (e == ESP_OK && out && len) {
    int got = (int)x->actual_num_bytes - 8;
    if (got < 0)
      got = 0;
    if (got > (int)len)
      got = len;
    memcpy(out, x->data_buffer + 8, (size_t)got);
    if (out_len)
      *out_len = got;
  }
  usb_host_transfer_free(x);
  return e;
}

/* UAC SET_CUR helpers (class request to the AudioControl interface). */
static esp_err_t uac_set_clock_hz(uint8_t clk_id, uint32_t hz) {
  uint8_t d[4] = {(uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)(hz >> 16),
                  (uint8_t)(hz >> 24)};
  /* CS_SAM_FREQ_CONTROL (0x01) << 8 */
  return ctrl_xfer_sync(0x21, 0x01, 0x0100,
                        (uint16_t)((clk_id << 8) | s_ac_iface), d, 4);
}
/* UAC2: read the Clock Source's supported rates (RANGE on SAM_FREQ, layout-3
 * subranges {dMIN,dMAX,dRES} x u32) and pick 44.1 k native > 48 k resampled >
 * first advertised — mirroring the UAC1 tSamFreq parse. Do NOT infer support
 * from SET_CUR acceptance: rate-locked devices ACK 44100 and then play
 * silence (the UAC1 Sony failure, UAC2 edition). Returns 0 on query failure
 * (caller keeps OUTPUT_RATE — the prior behavior). */
static uint32_t uac2_clock_pick_rate(void) {
  uint8_t buf[2 + 12 * 8]; /* up to 8 subranges */
  int got = 0;
  if (ctrl_xfer_sync_in(0xA1, 0x02 /* RANGE */, 0x0100 /* SAM_FREQ */,
                        (uint16_t)((s_clock_id << 8) | s_ac_iface), buf,
                        sizeof(buf), &got) != ESP_OK ||
      got < 2)
    return 0;
  int n = buf[0] | (buf[1] << 8);
  if (n <= 0)
    return 0;
  if (n > (got - 2) / 12)
    n = (got - 2) / 12; /* device sent fewer subranges than it claimed */
  uint32_t first_min = 0;
  bool has_chosen = false, has_44100 = false, has_48000 = false;
  for (int i = 0; i < n; i++) {
    const uint8_t *r = buf + 2 + i * 12;
    uint32_t mn =
        r[0] | (r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
    uint32_t mx =
        r[4] | (r[5] << 8) | ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
    uint32_t res =
        r[8] | (r[9] << 8) | ((uint32_t)r[10] << 16) | ((uint32_t)r[11] << 24);
    if (i == 0)
      first_min = mn;
    if (s_chosen_rate >= mn && s_chosen_rate <= mx &&
        (res == 0 || (s_chosen_rate - mn) % res == 0))
      has_chosen = true;
    if (44100 >= mn && 44100 <= mx && (res == 0 || (44100 - mn) % res == 0))
      has_44100 = true;
    if (48000 >= mn && 48000 <= mx && (res == 0 || (48000 - mn) % res == 0))
      has_48000 = true;
  }
  /* The user-chosen rate wins when the clock lists it; otherwise fall back
   * 44.1 k native > 48 k resampled > first advertised. */
  if (has_chosen)
    return s_chosen_rate;
  if (has_44100)
    return 44100;
  if (has_48000)
    return 48000;
  return first_min;
}

/* UAC1: SAMPLING_FREQ_CONTROL on the streaming ENDPOINT (3-byte rate). The EP
 * exists only after the streaming alt is selected, so call after SET_INTERFACE.
 */
static esp_err_t uac1_set_ep_rate(uint8_t ep, uint32_t hz) {
  uint8_t d[3] = {(uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)(hz >> 16)};
  return ctrl_xfer_sync(0x22, 0x01, 0x0100, ep, d, 3);
}
static esp_err_t uac_fu_set_mute(uint8_t fu_id, bool mute) {
  uint8_t d = mute ? 1 : 0; /* MUTE_CONTROL (0x01) << 8, channel 0 (master) */
  return ctrl_xfer_sync(0x21, 0x01, 0x0100,
                        (uint16_t)((fu_id << 8) | s_ac_iface), &d, 1);
}
static esp_err_t uac_fu_set_volume_db(uint8_t fu_id, int16_t db_q8) {
  uint8_t d[2] = {(uint8_t)db_q8, (uint8_t)(db_q8 >> 8)};
  uint16_t widx = (uint16_t)((fu_id << 8) | s_ac_iface);
  /* Write EVERY channel, always: the KEF EGG's FU declares MUTE on master
   * (ch0) and VOLUME only on ch1(L)/ch2(R) — it ACCEPTS master writes but
   * ignores them, leaving the real gain stuck at the device's stored level.
   * Windows writes the channels its driver finds, which is why PC is loud
   * and we were quiet. Master first (covers standard devices), then L+R
   * unconditionally. */
  esp_err_t e = ctrl_xfer_sync(0x21, 0x01, 0x0200, widx, d, 2); /* ch0 */
  esp_err_t l = ctrl_xfer_sync(0x21, 0x01, 0x0201, widx, d, 2); /* ch1 = L */
  esp_err_t r = ctrl_xfer_sync(0x21, 0x01, 0x0202, widx, d, 2); /* ch2 = R */
  if (e == ESP_OK || l == ESP_OK || r == ESP_OK)
    return ESP_OK;
  return e;
}
static esp_err_t uac_fu_get_volume_ch_db(uint8_t fu_id, uint8_t ch, float *db) {
  /* GET_CUR VOLUME on a specific channel (0=master, 1=L, 2=R) — the EGG
   * stores its real gain on ch1/ch2, so the master readback we used before
   * only echoed our own (ignored) writes. */
  uint8_t d[2];
  int got = 0;
  uint16_t widx = (uint16_t)((fu_id << 8) | s_ac_iface);
  esp_err_t e = ctrl_xfer_sync_in(0xa1, 0x81, (uint16_t)(0x0200 | (ch & 0x7f)),
                                  widx, d, sizeof(d), &got);
  if (e != ESP_OK || got < 2)
    return ESP_FAIL;
  *db = (float)(int16_t)(d[0] | (d[1] << 8)) / 256.0f;
  return ESP_OK;
}
static esp_err_t uac_fu_get_volume_db(uint8_t fu_id, float *db) {
  /* GET_CUR VOLUME (UAC1: 2-byte 8.8 signed fixed point, dB). Master channel
   * first; some devices only answer per-channel — mirror the setter's
   * fallback (L channel as representative). */
  uint8_t d[2];
  int got = 0;
  uint16_t widx = (uint16_t)((fu_id << 8) | s_ac_iface);
  esp_err_t e = ctrl_xfer_sync_in(0xa1, 0x81, 0x0200, widx, d, sizeof(d), &got);
  if (e != ESP_OK || got < 2) {
    got = 0;
    e = ctrl_xfer_sync_in(0xa1, 0x81, 0x0201, widx, d, sizeof(d), &got);
  }
  if (e != ESP_OK || got < 2) {
    return ESP_FAIL;
  }
  *db = (float)(int16_t)(d[0] | (d[1] << 8)) / 256.0f;
  return ESP_OK;
}

static esp_err_t uac_fu_get_volume_range(uint8_t fu_id, float *min_db,
                                         float *max_db) {
  uint8_t lo[2], hi[2];
  int n = 0;
  uint16_t widx = (uint16_t)((fu_id << 8) | s_ac_iface);
  esp_err_t a = ctrl_xfer_sync_in(0xa1, 0x82, 0x0200, widx, lo, sizeof(lo), &n);
  if (a == ESP_OK && n < 2) {
    a = ESP_FAIL;
  }
  n = 0;
  esp_err_t b = ctrl_xfer_sync_in(0xa1, 0x83, 0x0200, widx, hi, sizeof(hi), &n);
  if (b == ESP_OK && n < 2) {
    b = ESP_FAIL;
  }
  if (a != ESP_OK || b != ESP_OK) {
    return ESP_FAIL;
  }
  *min_db = (float)(int16_t)(lo[0] | (lo[1] << 8)) / 256.0f;
  *max_db = (float)(int16_t)(hi[0] | (hi[1] << 8)) / 256.0f;
  return ESP_OK;
}

/* Rebuild the per-FU info table and snapshot each FU's volume BEFORE any of
 * our writes (the factory state shows which FU really carries the volume). */
static void fu_info_refresh_defaults(void) {
  s_fu_info_n = s_fu_count;
  for (int i = 0; i < s_fu_count; i++) {
    fu_info_t *fi = &s_fu_info[i];
    fi->id = s_fu_ids[i];
    fi->src = s_fu_src[i];
    fi->is_spk = (s_fu_ids[i] == s_speaker_src) ||
                 (s_usb_it && s_fu_src[i] == s_usb_it);
    fi->is_mic = (s_fu_ids[i] == s_mic_fu);
    fi->cur_db = 0.0f;
    fi->read_ok =
        (uac_fu_get_volume_db(s_fu_ids[i], &fi->default_db) == ESP_OK);
  }
}

/* Write the device volume to EVERY speaker-path FU (all FUs on mic-less
 * cards — e.g. the KEF EGG — because the topology guess may have missed the
 * one that is actually in the audible path). The mic FU always stays at 0. */
static void set_device_fu_volume(float db) {
  float w = db;
  if (s_fu_max_db != 0.0f && w > s_fu_max_db) {
    w = s_fu_max_db;
  }
  if (s_fu_min_db != 0.0f && w < s_fu_min_db) {
    w = s_fu_min_db;
  }
  int16_t q8 = (int16_t)(w * 256.0f);
  bool no_mic = (s_mic_fu == 0);
  for (int i = 0; i < s_fu_count; i++) {
    if (s_fu_info[i].is_mic) {
      continue;
    }
    if (!no_mic && !s_fu_info[i].is_spk) {
      continue; /* sidetone hygiene on headsets */
    }
    uac_fu_set_volume_db(s_fu_ids[i], q8);
  }
  s_fu_vol_db = w;
}

/* One-shot diagnostic: does the card's FU accept SET_CUR and return GET_CUR?
 * Does it have positive headroom above 0 dB? Write 0 dB -> read back, write
 * +6 dB -> read back, then restore the persisted device volume. Results go to
 * the serial log AND into s_fu_probe, which the web UI shows — so a user
 * without a serial connection can still read the outcome on the config page.
 * (Probing at boot is safe: the stream is silence at that point, and +6 dB is
 * restored within milliseconds.) */
static void probe_fu_volume(uint8_t fu_id, float restore_db) {
  float cur = 0.0f;
  float mn = 0.0f, mx = 0.0f;
  fu_volume_probe_t *pr = &s_fu_probe;
  memset(pr, 0, sizeof(*pr));
  pr->probe_ran = true;

  pr->range_ok = (uac_fu_get_volume_range(fu_id, &mn, &mx) == ESP_OK);
  if (pr->range_ok) {
    pr->min_db = mn;
    pr->max_db = mx;
    ESP_LOGI(TAG, "FU %u volume range: %.1f..%.1f dB", fu_id, mn, mx);
  } else {
    ESP_LOGI(TAG, "FU %u range GET_MIN/MAX failed", fu_id);
  }

  pr->set0_ok = (uac_fu_set_volume_db(fu_id, 0) == ESP_OK);
  pr->get0_ok = (uac_fu_get_volume_db(fu_id, &cur) == ESP_OK);
  pr->get0_db = pr->get0_ok ? cur : 0.0f;
  ESP_LOGI(TAG, "FU %u SET 0 dB: %s | GET back: %s (%.1f dB)", fu_id,
           pr->set0_ok ? "ok" : "FAIL", pr->get0_ok ? "ok" : "FAIL",
           pr->get0_db);

  /* Climb +6, +12, ... until the readback stops rising — that plateau is the
   * card's real ceiling (the device clamps writes above its range, so this is
   * safe and finds the top even when GET_MIN/GET_MAX is refused). */
  float hi = pr->get0_db;
  pr->set6_ok = (uac_fu_set_volume_db(fu_id, 6 * 256) == ESP_OK);
  pr->get6_ok = (uac_fu_get_volume_db(fu_id, &cur) == ESP_OK);
  pr->get6_db = pr->get6_ok ? cur : 0.0f;
  ESP_LOGI(TAG, "FU %u SET +6 dB: %s | GET back: %s (%.1f dB)  [positive "
           "headroom probe]", fu_id, pr->set6_ok ? "ok" : "FAIL",
           pr->get6_ok ? "ok" : "FAIL", pr->get6_db);
  if (pr->get6_ok) {
    hi = pr->get6_db;
  }
  for (int v = 12; v <= 72 && hi >= 0.0f; v += 12) {
    if (uac_fu_set_volume_db(fu_id, v * 256) != ESP_OK) {
      break;
    }
    float r = 0.0f;
    if (uac_fu_get_volume_db(fu_id, &r) != ESP_OK || r <= hi) {
      break; /* clamped at hi — that is the ceiling */
    }
    hi = r;
    ESP_LOGI(TAG, "  FU %u holds +%d dB (readback %.1f)", fu_id, v, r);
  }
  pr->max_probe_db = hi;

  esp_err_t wr = uac_fu_set_volume_db(fu_id, (int16_t)(restore_db * 256.0f));
  float rr = 0.0f;
  esp_err_t gr = uac_fu_get_volume_db(fu_id, &rr);
  ESP_LOGI(TAG, "FU %u restore %.1f dB: SET %s | GET %s (%.1f dB) | ceiling "
           "%.1f dB", fu_id, restore_db, esp_err_to_name(wr),
           esp_err_to_name(gr), gr == ESP_OK ? rr : 0.0f, pr->max_probe_db);
}

void audio_output_get_fu_probe(fu_volume_probe_t *out) {
  if (out) {
    *out = s_fu_probe;
  }
}

int audio_output_get_fu_info(fu_info_t *out, int max) {
  int n = s_fu_info_n;
  if (n > max) {
    n = max;
  }
  for (int i = 0; i < n; i++) {
    out[i] = s_fu_info[i];
  }
  return n;
}

int audio_output_get_alt_info(usb_alt_info_t *out, int max) {
  int n = s_alt_count;
  if (n > max) {
    n = max;
  }
  for (int i = 0; i < n; i++) {
    out[i] = s_alt_info[i];
  }
  return n;
}

static esp_err_t uac_mixer_set_db(uint8_t mixer_id, uint8_t in_ch,
                                  uint8_t out_ch, int16_t db_q8) {
  uint8_t d[2] = {(uint8_t)db_q8, (uint8_t)(db_q8 >> 8)};
  /* wValue = (input channel number << 8) | output channel number */
  return ctrl_xfer_sync(0x21, 0x01, (uint16_t)((in_ch << 8) | out_ch),
                        (uint16_t)((mixer_id << 8) | s_ac_iface), d, 2);
}

/* ── HID media buttons (headset play/volume keys) ────────────────────────── */
typedef enum {
  HID_ACT_PLAY_PAUSE,
  HID_ACT_VOL_UP,
  HID_ACT_VOL_DOWN,
  HID_ACT_PREV,
  HID_ACT_NEXT,
} hid_action_t;

/* DACP (play/pause, volume) does mDNS + HTTP, which can block — so the USB
 * callback only enqueues here; this task does the network work. */
static void hid_action_task(void *arg) {
  (void)arg;
  int v;
  while (1) {
    if (xQueueReceive(s_hid_action_q, &v, portMAX_DELAY) != pdTRUE)
      continue;
    switch ((hid_action_t)v) {
    case HID_ACT_PLAY_PAUSE:
      ESP_LOGI(TAG, "HID button -> play/pause");
      playback_control_play_pause();
      break;
    case HID_ACT_VOL_UP:
      /* Mirror Windows: the remote's volume keys move the HOST-side volume
       * (Windows volume slider there, our device volume here). DACP (phone
       * volume) is deliberately NOT touched — the phone's own slider stays
       * independent. The web slider polls device_volume_db, so it moves
       * along, giving visible feedback like the Windows slider does. */
      ESP_LOGI(TAG, "HID button -> device volume up");
      audio_output_set_device_volume_db(
          audio_output_get_device_volume_db() + 2.0f);
      break;
    case HID_ACT_VOL_DOWN:
      ESP_LOGI(TAG, "HID button -> device volume down");
      audio_output_set_device_volume_db(
          audio_output_get_device_volume_db() - 2.0f);
      break;
    case HID_ACT_PREV:
      ESP_LOGI(TAG, "HID button -> previous track");
      playback_control_prev();
      break;
    case HID_ACT_NEXT:
      ESP_LOGI(TAG, "HID button -> next track");
      playback_control_next();
      break;
    }
  }
}

static void hid_post(hid_action_t a) {
  int v = (int)a;
  s_hid_events++;
  if (s_hid_action_q)
    xQueueSend(s_hid_action_q, &v, 0); /* non-blocking */
}

static void hid_in_cb(usb_transfer_t *xfer) {
  if (!s_hid_active)
    return;
  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED &&
      xfer->actual_num_bytes > 0) {
    const uint8_t *r = xfer->data_buffer;
    int nb = xfer->actual_num_bytes;
    /* Layout (Apple EarPods + similar): byte[0]=report ID, byte[1]=button
     * bitmap. Keep the raw log (only fires on press/release) to support other
     * devices, then dispatch once per new press (0->1 edge). */
    ESP_LOGI(TAG, "HID report (%dB): %02x %02x %02x %02x", nb, r[0],
             nb > 1 ? r[1] : 0, nb > 2 ? r[2] : 0, nb > 3 ? r[3] : 0);
    uint8_t bm = (nb > 1) ? r[1] : 0;
    uint8_t b2 = (nb > 2) ? r[2] : 0;
    uint8_t newly = (uint8_t)(bm & ~s_hid_prev);
    uint8_t newly2 = (uint8_t)(b2 & ~s_hid_prev_b2); /* compute BEFORE prev update */
    /* Snapshot the raw report at any PRESS edge (byte[1] OR byte[2] going
     * 0->nonzero) — the KEF EGG's play/pause may live in byte[2], and its
     * release report overwrites the last raw before the web UI can read it. */
    if ((bm != 0 && bm != s_hid_prev) || (b2 != 0 && b2 != s_hid_prev_b2)) {
      s_hid_press_n = (uint8_t)(nb > 4 ? 4 : nb);
      for (int i = 0; i < s_hid_press_n; i++) {
        s_hid_press_raw[i] = r[i];
      }
    }
    s_hid_prev = bm;
    s_hid_prev_b2 = b2;
    /* Keep the raw report for the web UI — the KEF EGG's volume-key layout
     * differs from Apple headsets; showing the bytes settles the mapping. */
    s_hid_last_n = (uint8_t)(nb > 4 ? 4 : nb);
    for (int i = 0; i < s_hid_last_n; i++) {
      s_hid_last_raw[i] = r[i];
    }
    /* Layout-aware dispatch: the byte[1] mapping differs between Apple
     * standard and KEF EGG; byte[2] transport keys are handled in both
     * (Apple headsets keep byte[2] zero, so no double-fire). */
    bool egg = settings_remote_layout_egg();
    if (egg) {
      if (newly & HID_EGG_VOL_UP) {
        s_hid_volup++;
        hid_post(HID_ACT_VOL_UP);
      }
      if (newly & HID_EGG_VOL_DOWN) {
        s_hid_voldown++;
        hid_post(HID_ACT_VOL_DOWN);
      }
    } else {
      if (newly & HID_AP_PLAY_PAUSE)
        hid_post(HID_ACT_PLAY_PAUSE);
      if (newly & HID_AP_VOL_UP) {
        s_hid_volup++;
        hid_post(HID_ACT_VOL_UP);
      }
      if (newly & HID_AP_VOL_DOWN) {
        s_hid_voldown++;
        hid_post(HID_ACT_VOL_DOWN);
      }
    }
    if (newly2 & HID_B2_PLAY_PAUSE)
      hid_post(HID_ACT_PLAY_PAUSE);
    if (newly2 & HID_B2_NEXT)
      hid_post(HID_ACT_NEXT);
    if (newly2 & HID_B2_PREV)
      hid_post(HID_ACT_PREV);
  }
  if (s_hid_active)
    usb_host_transfer_submit(xfer);
}

static esp_err_t start_hid(void) {
  if (usb_host_transfer_alloc(s_hid_mps, 0, &s_hid_urb) != ESP_OK)
    return ESP_ERR_NO_MEM;
  s_hid_urb->device_handle = s_dev;
  s_hid_urb->bEndpointAddress = s_hid_ep;
  s_hid_urb->callback = hid_in_cb;
  s_hid_urb->context = NULL;
  s_hid_urb->num_bytes = s_hid_mps;
  s_hid_active = true;
  return usb_host_transfer_submit(s_hid_urb);
}

static void teardown_device(void); /* used on unrecoverable setup failure */

/* Read the device descriptor's iProduct string (standard GET_DESCRIPTOR) into
 * s_product. Best-effort — many DACs omit it or respond to 0x0409 only. */
static void read_product_string(void) {
  s_product[0] = '\0';
  const usb_device_desc_t *dd = NULL;
  if (usb_host_get_device_descriptor(s_dev, &dd) != ESP_OK || !dd ||
      dd->iProduct == 0) {
    return;
  }
  uint8_t buf[2 + 64] = {0};
  int got = 0;
  if (ctrl_xfer_sync_in(0x80, 0x06, /* GET_DESCRIPTOR, standard, device */
                        (uint16_t)((0x03 << 8) | dd->iProduct), 0x0409, buf,
                        sizeof(buf), &got) != ESP_OK ||
      got < 2) {
    return;
  }
  int chars = (got - 2) / 2;
  if (chars <= 0) {
    return;
  }
  if (chars > (int)sizeof(s_product) - 1) {
    chars = (int)sizeof(s_product) - 1;
  }
  for (int i = 0; i < chars; i++) {
    uint16_t c = (uint16_t)(buf[2 + i * 2] | (buf[3 + i * 2] << 8));
    s_product[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
  }
  s_product[chars] = '\0';
  ESP_LOGI(TAG, "USB device product: %s", s_product);
}

static void setup_device(uint8_t addr) {
  s_setup_stage = 1;
  s_setup_err = 0;
  s_dev_addr = addr;
  /* v1.1.2: after a web-UI reprobe the stale handle's close is dispatched on
   * the client event loop; opening the same address a few ms later can race
   * that cleanup and fail with a generic error. Retry briefly — a real
   * hot-plug has already finished enumeration, so this only ever waits for
   * the stale close to drain. */
  esp_err_t oerr = ESP_FAIL;
  for (int attempt = 0; attempt < 5; attempt++) {
    oerr = usb_host_device_open(s_client, addr, &s_dev);
    if (oerr == ESP_OK)
      break;
    s_dev = NULL;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (oerr != ESP_OK) {
    ESP_LOGE(TAG, "device_open(addr=%u) failed after retries: %s", addr,
             esp_err_to_name(oerr));
    s_setup_stage = 1;
    s_setup_err = (int)oerr; /* real esp_err; positive value, e.g. 0x102 */
    return;
  }
  s_setup_stage = 2;
  read_product_string();
  /* v1.1: resolve the user-chosen rate x bits before parsing the descriptor —
   * the alt/rate preference below (and the UAC2 clock pick) both follow it. */
  resolve_chosen_format();
  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK || !cfg) {
    ESP_LOGE(TAG, "get_active_config_descriptor failed");
    s_setup_err = -2;
    goto fail_close;
  }
  /* Keep a raw copy for the /api/desc dump — diagnosing the KEF EGG's quiet
   * USB path needs the full AudioControl entity list (mixers, XUs, PUs),
   * not just the iso alts. */
  uint16_t cfg_total = cfg->wTotalLength;
  if (cfg_total > 0 && cfg_total <= sizeof(s_cfg_raw)) {
    memcpy(s_cfg_raw, cfg, cfg_total);
    s_cfg_raw_n = cfg_total;
  }
  s_setup_stage = 3;
  if (!find_speaker_altsetting(cfg)) {
    ESP_LOGE(TAG, "No stereo PCM iso OUT alt-setting found");
    s_setup_err = -3;
    goto fail_close;
  }
  s_setup_stage = 4;
  /* UAC2 rates live on the Clock Source, not in the AS descriptors, so the
   * alt selection above assumed OUTPUT_RATE. Ask the clock what it really
   * supports and re-pick before anything consumes s_out_rate — a rate-locked
   * UAC2 device otherwise gets SET_CUR(44100), ACKs it, and plays silence. */
  if (s_uac_ver == 2 && s_clock_id) {
    uint32_t r = uac2_clock_pick_rate();
    if (r && r != s_out_rate) {
      unsigned need = (unsigned)(r / 1000 + (r % 1000 != 0)) * 2u *
                      (unsigned)s_out_subslot; /* peak bytes per 1 ms */
      if (need <= s_out_mps) {
        ESP_LOGI(TAG, "UAC2 clock: device supports %lu Hz, not %d — using it",
                 (unsigned long)r, OUTPUT_RATE);
        s_out_rate = r;
      } else {
        ESP_LOGW(TAG, "UAC2 clock rate %lu needs %u B/pkt > mps %u — keeping",
                 (unsigned long)r, need, s_out_mps);
      }
    }
  }

  ESP_LOGI(TAG,
           "Speaker iface=%u alt=%u ep=0x%02x mps=%u rate=%lu | Mic iface=%u "
           "alt=%u ep=0x%02x ch=%d | AC iface=%u clocks=%d FUs=%d mic_fu=%u",
           s_out_iface, s_out_alt, s_out_ep, s_out_mps,
           (unsigned long)s_out_rate, s_in_iface, s_in_alt, s_in_ep,
           s_in_channels, s_ac_iface, s_clock_count, s_fu_count, s_mic_fu);

  /* 0) Mute every feature unit FIRST (best-effort — some FUs reject writes).
   * Between USB reset and the first iso packet the device's DAC is enabled
   * but unclocked, and several DACs emit white noise in that window. Keep
   * them muted through setup; unmute AFTER the silence stream is flowing
   * (step 5 below). */
  for (int i = 0; i < s_fu_count; i++)
    uac_fu_set_mute(s_fu_ids[i], true);

  /* 1) UAC2: set every clock source's sample rate BEFORE selecting a streaming
   * alt. UAC1 has no clock entities — its rate is set on the endpoint after
   * SET_INTERFACE (below). */
  if (s_uac_ver == 2) {
    for (int i = 0; i < s_clock_count; i++) {
      esp_err_t e = uac_set_clock_hz(s_clock_ids[i], s_out_rate);
      ESP_LOGI(TAG, "set clock id=%u %lu Hz: %s", s_clock_ids[i],
               (unsigned long)s_out_rate, esp_err_to_name(e));
    }
    if (s_clock_count == 0 && s_clock_id)
      uac_set_clock_hz(s_clock_id, s_out_rate);
  }

  /* 2) Claim the streaming interface+alt. NOTE: usb_host_interface_claim() only
   * configures the HOST-side pipes — it does NOT send SET_INTERFACE to the
   * device. Without the explicit SET_INTERFACE below, the device's streaming
   * endpoint stays on alt 0 (DISABLED) and every iso-OUT packet is silently
   * dropped (the host still reports pkt_err=0 because iso has no handshake). */
  if (usb_host_interface_claim(s_client, s_dev, s_out_iface, s_out_alt) !=
      ESP_OK) {
    ESP_LOGE(TAG, "interface_claim(iface=%u alt=%u) failed", s_out_iface,
             s_out_alt);
    s_setup_stage = 4;
    s_setup_err = -4;
    goto fail_close;
  }
  s_setup_stage = 5;
  /* The new device may run a different rate — flag the playback task (the
   * resampler's sole owner) to rebuild it at the top of its loop. Calling
   * audio_resample_init() from this task would free buffers a concurrent
   * audio_resample_process() may be reading (hot-plug during playback =
   * use-after-free). */
  resample_reinit_needed = true;
  /* 2b) Prime and submit the silence URBs BEFORE SET_INTERFACE. While the
   * device is on alt 0 it discards iso OUT data, so this is harmless — and
   * the moment SET_INTERFACE enables its endpoint, real (zero) data is
   * already arriving. Devices whose DAC hisses while enabled-but-unclocked
   * (startup white noise) get clocked immediately. */
  /* 2.5) PRE-STREAM FU work — probe the volume range and leave every speaker
   * FU at its maximum. Many devices sample the FU volume when the iso stream
   * goes active and then ignore later changes (the KEF EGG: post-stream
   * writes of 0..+72 dB changed nothing audible, and its readback even drifts
   * on its own). Driving the FU to its max BEFORE the stream starts is the
   * only way to get such a card into its loudest state — mimicking Windows,
   * which raises the device volume at connect. The web slider then controls a
   * digital gain (attenuation only) on top, so 100% = full-scale = the
   * loudest the card can be. */
  fu_info_refresh_defaults(); /* snapshot factory volumes BEFORE our writes */
  for (int i = 0; i < s_fu_count && !s_spk_fu; i++) {
    if (s_fu_info[i].is_spk)
      s_spk_fu = s_fu_ids[i]; /* primary speaker FU (probe target) */
  }
  if (s_spk_fu) {
    ESP_LOGI(TAG, "Pre-stream: probing speaker FU %u", s_spk_fu);
    probe_fu_volume(s_spk_fu, 100.0f); /* climb +6..+72 (diagnostic only) */
    /* Leave the FU at its DECLARED maximum (GET_MAX: 0 dB on KEF EGG), NOT at
     * the probe's out-of-range climb (+72). KEF EGG echoes any write (even
     * +100) without clamping, and an out-of-range gain may push its DSP into
     * limiting — Windows raises the device volume to 0 dB at connect, which
     * is the state we reproduce. */
    s_fu_max_db = s_fu_probe.max_db;
    set_device_fu_volume(s_fu_max_db > 0.0f ? s_fu_max_db : 0.0f);
    /* The EGG's real gain lives on ch1(L)/ch2(R) — capture what the device
     * actually holds AFTER our all-channel 0 dB write. If these read back
     * far below 0, the gain path was stuck at the stored level all along. */
    if (uac_fu_get_volume_ch_db(s_spk_fu, 1, &s_fu_ch1_db) != ESP_OK)
      s_fu_ch1_db = -100.0f; /* -100 = unreadable */
    if (uac_fu_get_volume_ch_db(s_spk_fu, 2, &s_fu_ch2_db) != ESP_OK)
      s_fu_ch2_db = -100.0f;
    ESP_LOGI(TAG, "FU ch1/ch2 readback after 0dB write: %.1f / %.1f dB",
             s_fu_ch1_db, s_fu_ch2_db);
  }
  s_setup_stage = 8;

  if (start_streaming() != ESP_OK) {
    /* A mid-loop alloc/submit failure leaves earlier URBs allocated and
     * possibly IN FLIGHT. Releasing the interface under them (the old
     * cleanup) wedges the device, and the next connect would overwrite
     * s_urb[] and orphan live transfers. teardown_device() waits them out
     * and frees everything. */
    ESP_LOGE(TAG, "start_streaming failed — teardown");
    s_setup_stage = 5;
    s_setup_err = -5;
    teardown_device();
    return;
  }
  s_setup_stage = 6;

  /* THE FIX: switch the DEVICE to the streaming alt (enables its endpoint).
   * Everything hangs on this transfer: if it doesn't land, the device stays
   * on alt 0 and every iso-OUT byte is silently discarded with zero error
   * feedback (iso has no handshake). Retry transient control-pipe failures;
   * on persistent failure tear down rather than run a provably silent
   * session that logs "streaming started". */
  esp_err_t si = ESP_FAIL;
  for (int attempt = 0; attempt < 3 && si != ESP_OK; attempt++) {
    if (attempt)
      vTaskDelay(pdMS_TO_TICKS(20));
    si = ctrl_xfer_sync(0x01, 0x0b, s_out_alt, s_out_iface, NULL, 0);
  }
  ESP_LOGI(TAG, "SET_INTERFACE(iface=%u alt=%u): %s", s_out_iface, s_out_alt,
           esp_err_to_name(si));
  if (si != ESP_OK) {
    ESP_LOGE(TAG, "device stuck on alt 0 (output would be silent) — teardown");
    s_setup_stage = 6;
    s_setup_err = -6;
    teardown_device();
    return;
  }
  s_setup_stage = 7;
  /* UAC1: now that the endpoint exists, set its sampling frequency. (Until
   * this lands the device consumes our zeros at its power-on rate — still
   * silence, so the order is safe.) */
  if (s_uac_ver == 1) {
    esp_err_t er = uac1_set_ep_rate(s_out_ep, s_out_rate);
    ESP_LOGI(TAG, "UAC1 set ep 0x%02x rate %lu Hz: %s", s_out_ep,
             (unsigned long)s_out_rate, esp_err_to_name(er));
    /* Read the rate back — the KEF EGG is suspected of staying in its 44.1 kHz
     * power-on mode when the SET_CUR is ignored, and 44.1 kHz is the state the
     * EGG itself plays quietly on (verified on Windows). Expose in the API so
     * a quiet unit can be checked without serial. */
    s_rate_readback = 0;
    uint8_t rb[3] = {0, 0, 0};
    int got = 0;
    if (ctrl_xfer_sync_in(0xa1, 0x81, 0x0100, s_out_ep, rb, sizeof(rb),
                          &got) == ESP_OK &&
        got == 3) {
      s_rate_readback = (uint32_t)rb[0] | ((uint32_t)rb[1] << 8) |
                        ((uint32_t)rb[2] << 16);
    }
    ESP_LOGI(TAG, "UAC1 ep rate readback: %lu Hz (asked %lu)",
             (unsigned long)s_rate_readback, (unsigned long)s_out_rate);
  }

  /* 3) Full-duplex: also claim the mic interface and run iso IN transfers, so
   * a headset that only routes USB audio during an active bidirectional call
   * un-gates its speaker. Best-effort — failure falls back to output-only. */
#if USB_HOST_FULL_DUPLEX
  if (s_in_ep) {
    esp_err_t ce =
        usb_host_interface_claim(s_client, s_dev, s_in_iface, s_in_alt);
    if (ce == ESP_OK && start_capture() == ESP_OK) {
      ESP_LOGI(TAG,
               "Full-duplex: mic IN iface=%u alt=%u ep=0x%02x ch=%d mps=%u",
               s_in_iface, s_in_alt, s_in_ep, s_in_channels, s_in_mps);
    } else {
      ESP_LOGW(TAG, "Full-duplex mic IN open failed (claim=%s) — OUT only",
               esp_err_to_name(ce));
      if (ce == ESP_OK)
        usb_host_interface_release(s_client, s_dev, s_in_iface);
      s_in_ep = 0;
    }
  }
#else
  s_in_ep = 0; /* output-only build: never open capture */
#endif

  ESP_LOGI(TAG, "USB audio OUT streaming started");
  s_setup_stage = 9;
  s_setup_err = 0;

  /* 4) NOW unmute — the silence stream is already flowing, so the device's
   * DAC is clocked with real (zero) data and can't hiss. Mic-less cards (the
   * KEF EGG) get EVERY FU unmuted — a topology guess can pick the wrong one
   * of several FUs, and leaving the real one muted keeps the speaker quiet no
   * matter what volume we write. On headsets (sidetone FUs exist) only the
   * speaker-path and mic-path FUs are unmuted, as before. */
  bool no_mic = (s_mic_fu == 0);
  for (int i = 0; i < s_fu_count; i++) {
    bool master = s_fu_info[i].is_spk;
    bool mic = s_fu_info[i].is_mic;
    if (!no_mic && !master && !mic) {
      continue; /* sidetone hygiene on headsets */
    }
    if (master && s_spk_fu == 0) {
      s_spk_fu = s_fu_ids[i]; /* primary speaker FU (probe target) */
    }
    esp_err_t em = uac_fu_set_mute(s_fu_ids[i], false);
    ESP_LOGI(TAG, "FU %u %s mute=0:%s", s_fu_ids[i],
             master ? "SPK" : (mic ? "MIC" : "ALL"),
             esp_err_to_name(em));
  }
  ESP_LOGI(TAG, "Speaker FU = %u (volume already maxed pre-stream)", s_spk_fu);

  /* 5b) Route the USB stereo input through the mixer to the speaker (a headset
   * mixer's USB crosspoints default to muted, while the mic pin is unity). */
  if (s_mixer_id) {
    esp_err_t a = uac_mixer_set_db(s_mixer_id, 1, 1, 0); /* USB L -> out L */
    esp_err_t b = uac_mixer_set_db(s_mixer_id, 2, 2, 0); /* USB R -> out R */
    ESP_LOGI(TAG, "mixer %u (1->1):%s (2->2):%s", s_mixer_id,
             esp_err_to_name(a), esp_err_to_name(b));
  }

  /* 6) Headset media buttons: claim the HID interface (if present) and read its
   * interrupt reports. Best-effort — failure just means no button control. */
  if (s_hid_iface != 0xff && s_hid_ep) {
    if (usb_host_interface_claim(s_client, s_dev, s_hid_iface, s_hid_alt) ==
        ESP_OK) {
      ctrl_xfer_sync(0x21, 0x0a, 0x0000, s_hid_iface, NULL, 0); /* SET_IDLE 0 */
      if (start_hid() == ESP_OK) {
        ESP_LOGI(TAG, "HID media buttons active (iface=%u ep=0x%02x)",
                 s_hid_iface, s_hid_ep);
      } else {
        usb_host_interface_release(s_client, s_dev, s_hid_iface);
        s_hid_iface = 0xff;
      }
    } else {
      ESP_LOGW(TAG, "HID claim failed (iface=%u)", s_hid_iface);
      s_hid_iface = 0xff;
    }
  }
  return;

fail_close:
  usb_host_device_close(s_client, s_dev);
  s_dev = NULL;
}

static void teardown_device(void) {
  s_streaming = false;
  s_capturing = false;
  s_hid_active = false;
  s_hid_events = 0;
  s_hid_volup = s_hid_voldown = 0;
  s_hid_last_n = 0;
  /* Forget the speaker FU: web volume falls back to software gain and the
   * poll no longer touches the vanished device's control pipe. */
  s_spk_fu = 0;
  s_fu_min_db = s_fu_max_db = 0.0f;
  s_fu_info_n = 0;
  flush_requested = true; /* playback task drops the now-stale FIFO bytes */
  /* Wait out in-flight URBs before freeing them: the staggered iso-OUT ring
   * spans NUM_URBS * PACKETS_PER_URB ms (32 ms) on a still-ALIVE device
   * (SET_INTERFACE-failure teardown), and callbacks stop resubmitting only
   * once s_streaming is false. usb_host_transfer_free() on an in-flight URB
   * corrupts the heap. On DEV_GONE the stack fails transfers back much
   * faster — the extra wait just makes disconnect teardown unhurried. */
  vTaskDelay(pdMS_TO_TICKS(NUM_URBS * PACKETS_PER_URB + 10));
  /* v1.1.3: usb_host_transfer_free() fails on an in-flight URB. A leaked
   * in-flight URB makes interface_release() (endpoint busy) and then
   * device_close() fail; device_close() then keeps the client's 'device
   * opened' record set, so the next device_open() fails forever (reprobe
   * stuck at 'enumeration incomplete'). Poll free until it succeeds instead
   * of assuming the fixed wait above was enough. */
  for (int i = 0; i < NUM_URBS; i++) {
    if (s_urb[i]) {
      for (int a = 0; a < 100; a++) { /* up to ~1 s */
        if (usb_host_transfer_free(s_urb[i]) == ESP_OK)
          break;
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      s_urb[i] = NULL;
    }
  }
  for (int i = 0; i < NUM_IN_URBS; i++) {
    if (s_in_urb[i]) {
      for (int a = 0; a < 100; a++) {
        if (usb_host_transfer_free(s_in_urb[i]) == ESP_OK)
          break;
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      s_in_urb[i] = NULL;
    }
  }
  if (s_hid_urb) {
    for (int a = 0; a < 100; a++) {
      if (usb_host_transfer_free(s_hid_urb) == ESP_OK)
        break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_hid_urb = NULL;
  }
  if (s_dev) {
    esp_err_t e_rel = usb_host_interface_release(s_client, s_dev, s_out_iface);
    if (e_rel != ESP_OK)
      ESP_LOGE(TAG, "release out iface %u failed: %s", s_out_iface,
               esp_err_to_name(e_rel));
    if (s_in_ep) {
      e_rel = usb_host_interface_release(s_client, s_dev, s_in_iface);
      if (e_rel != ESP_OK)
        ESP_LOGE(TAG, "release in iface %u failed: %s", s_in_iface,
                 esp_err_to_name(e_rel));
    }
    if (s_hid_iface != 0xff) {
      e_rel = usb_host_interface_release(s_client, s_dev, s_hid_iface);
      if (e_rel != ESP_OK)
        ESP_LOGE(TAG, "release hid iface %u failed: %s", s_hid_iface,
                 esp_err_to_name(e_rel));
    }
    esp_err_t e_cls = usb_host_device_close(s_client, s_dev);
    if (e_cls != ESP_OK) {
      /* Keep the handle: s_dev stays set so a later open is not attempted
       * against a client record that still marks this device as opened. */
      ESP_LOGE(TAG, "device_close failed: %s — keeping handle", esp_err_to_name(e_cls));
      return;
    }
    s_dev = NULL;
  }
  s_in_ep = 0;
  s_hid_iface = 0xff;
  s_hid_ep = 0;
  s_product[0] = '\0';
  ESP_LOGI(TAG, "USB audio device torn down");
}

/* ── USB client (NEW_DEV / DEV_GONE) ─────────────────────────────────────── */
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    /* Latch unconditionally — NEW_DEV is edge-triggered and never retried.
     * On a fast unplug/replug (or a device that resets itself) it can
     * arrive while the old device's teardown hasn't cleared s_dev yet;
     * gating on !s_dev here dropped the event and left the device dead
     * until reboot. The setup task acts on the latch only once s_dev is
     * free; a newer plug simply overwrites the latched address. */
    s_pending_addr = msg->new_dev.address;
    s_connect_pending = true;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    s_disconnect_pending = true;
  }
}

static void usb_client_task(void *arg) {
  const usb_host_client_config_t cfg = {
      .is_synchronous = false,
      .max_num_event_msg = 5,
      .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
  };
  if (usb_host_client_register(&cfg, &s_client) != ESP_OK) {
    ESP_LOGE(TAG, "client_register failed");
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "USB host client ready; waiting for a USB speaker/headphone");
  /* Pump events continuously: dispatches NEW_DEV / DEV_GONE, the setup task's
   * control-transfer completions, and the iso OUT completions. */
  while (true)
    usb_host_client_handle_events(s_client, portMAX_DELAY);
}

/* Device setup/teardown runs on its own task (not the client task) so the
 * blocking control transfers in setup_device() can wait while the client task
 * keeps dispatching their completion callbacks. */
static void usb_setup_task(void *arg) {
  (void)arg;
  while (true) {
    if (s_disconnect_pending) {
      s_disconnect_pending = false;
      teardown_device();
    }
    /* Act on a latched NEW_DEV only when no device is open — teardown above
     * runs first, so a replug connects in this same iteration. If the
     * latched device vanished meanwhile, setup_device()'s open fails and
     * cleans up harmlessly. */
    if (s_connect_pending && s_client && !s_dev) {
      s_connect_pending = false;
      setup_device(s_pending_addr);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/* Graceful-reboot hook. A software reset (OTA, web-UI restart) kills the host
 * mid-stream; a bus-powered DAC that keeps VBUS through the reboot then hisses
 * on the dead bus until the next boot re-enumerates it (~7 s). Mute the FUs
 * and switch the streaming interface back to alt 0 (endpoint disabled) so the
 * device idles silently instead. Hard resets (button/power) can't run this. */
static void usb_audio_shutdown(void) {
  if (!s_dev)
    return;
  for (int i = 0; i < s_fu_count; i++)
    uac_fu_set_mute(s_fu_ids[i], true);
  ctrl_xfer_sync(0x01, 0x0b, 0, s_out_iface, NULL, 0); /* SET_INTERFACE alt 0 */
}

/* ── Volume + playback task ──────────────────────────────────────────────── */
/*
 * Two independent volume stages:
 *   1) AirPlay / source volume (airplay_get_volume_q15) — set by the phone
 *      during the session; it never touches the device volume storage.
 *   2) Device volume — the web slider / hardware buttons. With a USB sound
 *      card attached this is the CARD's own volume (UAC Feature Unit), the
 *      same stage the card's remote control adjusts: the slider writes it via
 *      SET_CUR and the web poll reads it back via GET_CUR, so remote changes
 *      show up in the web UI. Without a card it falls back to a software
 *      gain. The phone cannot overwrite it.
 */
static volatile int32_t s_peak = 0;  /* peak |sample| since last read */

static void recompute_device_q15(void) {
  /* ALWAYS a digital gain: 0 dB = unity (full-scale pass-through, exactly the
   * signal Windows sends at 100% system volume), below = attenuation. The
   * card's FU is NOT used for runtime volume — KEF EGG ignores FU writes
   * while streaming (proved by 0..+72 dB having no audible effect), and
   * writing +72 during streaming could stall the control pipe. The FU is
   * driven to its maximum BEFORE the stream starts (see setup step 2.5). */
  float lin = powf(10.0f, s_device_db / 20.0f);
  s_device_q15 = (int32_t)(lin * 32768.0f);
}

static void apply_volume(int16_t *buf, size_t n) {
#ifndef CONFIG_DAC_CONTROLS_VOLUME
  int32_t vol = airplay_get_volume_q15(); /* source/phone volume */
  /* × device gain. int64 keeps the math safe on int16 PCM. */
  vol = (int32_t)(((int64_t)vol * s_device_q15) >> 15);
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = buf[i];
    if (s < 0) {
      s = -s;
    }
    if (s > peak) {
      peak = s;
    }
    buf[i] = (int16_t)(((int64_t)buf[i] * vol) >> 15);
  }
  if (peak > s_peak) {
    s_peak = peak;
  }
#endif
}

void audio_output_set_device_volume_db(float volume_db) {
  if (volume_db < -30.0f) {
    volume_db = -30.0f;
  }
  if (volume_db > 0.0f) {
    volume_db = 0.0f;
  }
  s_device_db = volume_db;
  /* Digital gain only — the card's FU is driven to its max before the stream
   * starts and is NOT written while streaming (KEF EGG ignores runtime FU
   * writes; writing positive values there could also stall its control pipe).
   * 0 dB = full-scale pass-through, same as Windows at 100%. */
  recompute_device_q15();
  ESP_LOGI(TAG, "Device volume: %.1f dB (digital gain)", s_device_db);
}

float audio_output_get_device_volume_db(void) {
  /* The digital device volume (what the web slider controls / what the HID
   * remote keys adjust). The card's own FU readback is shown separately in
   * the FU table — it is NOT the device volume. */
  return s_device_db;
}

/* Read the card's current FU volume (GET_CUR) into the live value used by the
 * web UI. Called from the web status poll, so a remote-control change shows up
 * in the web slider. Persists the change so the boot-time FU write matches. */
void audio_output_refresh_fu_volume(void) {
  static uint32_t s_last_fail_log = 0;
  if (!s_spk_fu) {
    return;
  }
  /* Live-read EVERY FU so the web table shows which one actually moves
   * (that is how the remote's true volume control is identified). */
  for (int i = 0; i < s_fu_info_n; i++) {
    float rd = 0.0f;
    if (uac_fu_get_volume_db(s_fu_info[i].id, &rd) == ESP_OK) {
      s_fu_info[i].cur_db = rd;
      s_fu_info[i].read_ok = true;
    } else {
      s_fu_info[i].read_ok = false;
    }
  }
  /* The 设备音量 display follows the primary speaker FU. */
  float rd = s_fu_vol_db;
  esp_err_t e = uac_fu_get_volume_db(s_spk_fu, &rd);
  if (e != ESP_OK) {
    s_fu_probe.get_fail_count++;
    /* Log GET failures at most ~once a minute (the browser polls every 3 s). */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (now - s_last_fail_log > 60) {
      s_last_fail_log = now;
      ESP_LOGW(TAG, "FU %u volume GET_CUR failed: %s (card rejects "
                   "host volume readback)", s_spk_fu, esp_err_to_name(e));
    }
    return;
  }
  s_fu_probe.get_ever_ok = true;
  if (rd != s_fu_vol_db) {
    if (rd < -30.0f) {
      rd = -30.0f;
    }
    if (rd > 0.0f) {
      rd = 0.0f;
    }
    s_fu_vol_db = rd;
    s_device_db = rd;
    settings_set_volume(s_device_db);
    ESP_LOGI(TAG, "FU volume readback: %.1f dB (persisted)", s_device_db);
  }
}

float audio_output_get_fu_min_db(void) { return s_fu_min_db; }
float audio_output_get_fu_max_db(void) { return s_fu_max_db; }

uint16_t audio_output_get_config_raw(const uint8_t **buf) {
  if (buf)
    *buf = s_cfg_raw;
  return s_cfg_raw_n;
}
float audio_output_get_fu_vol_db(void) { return s_fu_vol_db; }
bool audio_output_has_fu_volume(void) { return s_spk_fu != 0; }

float audio_output_get_peak_dbfs(void) {
  int32_t p = s_peak;
  s_peak = 0;
  if (p <= 0) {
    return -99.0f;
  }
  return 20.0f * log10f((float)p / 32768.0f);
}

static volatile audio_channel_mode_t channel_mode = AUDIO_CHANNEL_STEREO;

/* Apply the selected channel mode to an interleaved stereo buffer (L,R,...).
 * LEFT/RIGHT route the chosen source channel to BOTH outputs so the selected
 * track is heard from both speakers; STEREO leaves the buffer untouched. */
static void apply_channel_mode(int16_t *buf, size_t frames) {
  audio_channel_mode_t mode = channel_mode;
  if (mode == AUDIO_CHANNEL_STEREO)
    return;
  size_t src = (mode == AUDIO_CHANNEL_RIGHT) ? 1 : 0;
  for (size_t i = 0; i < frames; i++) {
    int16_t s = buf[i * 2 + src];
    buf[i * 2] = s;
    buf[i * 2 + 1] = s;
  }
}

audio_channel_mode_t audio_output_cycle_channel_mode(void) {
  audio_channel_mode_t next;
  switch (channel_mode) {
  case AUDIO_CHANNEL_STEREO:
    next = AUDIO_CHANNEL_LEFT;
    break;
  case AUDIO_CHANNEL_LEFT:
    next = AUDIO_CHANNEL_RIGHT;
    break;
  default:
    next = AUDIO_CHANNEL_STEREO;
    break;
  }
  channel_mode = next;
  ESP_LOGI(TAG, "Channel mode: %s",
           next == AUDIO_CHANNEL_LEFT    ? "LEFT only"
           : next == AUDIO_CHANNEL_RIGHT ? "RIGHT only"
                                         : "STEREO");
  return next;
}

audio_channel_mode_t audio_output_get_channel_mode(void) {
  return channel_mode;
}

void audio_output_set_channel_mode(audio_channel_mode_t mode) {
  /* apply_channel_mode() implements STEREO/LEFT/RIGHT only; MONO would fall
   * through to the LEFT path, so clamp it instead of mislabelling it. */
  if (mode != AUDIO_CHANNEL_LEFT && mode != AUDIO_CHANNEL_RIGHT) {
    mode = AUDIO_CHANNEL_STEREO;
  }
  channel_mode = mode;
  ESP_LOGI(TAG, "Channel mode: %s",
           mode == AUDIO_CHANNEL_LEFT    ? "LEFT only"
           : mode == AUDIO_CHANNEL_RIGHT ? "RIGHT only"
                                         : "STEREO");
}

/* Routing is done in software here, so nothing fixes it the way a multi-device
 * DAC configuration does. Without this the weak default in
 * audio_output_common.c would report the mode as locked, greying the control
 * out in the web UI while the hardware button still cycled it. */
bool audio_output_channel_mode_locked(void) {
  return false;
}

static volatile int source_rate = 44100;

/* Expand n16 16-bit PCM samples to the device's subslot size, left-justified
 * (e.g. 16-bit -> 24-bit = value << 8). Writes n16 * subslot bytes. */
static int expand_pcm(const int16_t *src, int n16, uint8_t *dst, int subslot) {
  int shift = 8 * (subslot - 2);
  uint8_t *o = dst;
  for (int i = 0; i < n16; i++) {
    int32_t v = (int32_t)src[i] << shift;
    for (int b = 0; b < subslot; b++)
      *o++ = (uint8_t)(v >> (8 * b));
  }
  return n16 * subslot;
}

static void playback_task(void *arg) {
  int16_t *pcm = malloc((size_t)(FRAME_SAMPLES + 1) * 2 * sizeof(int16_t));
  int16_t *silence = calloc((size_t)FRAME_SAMPLES * 2, sizeof(int16_t));
  int16_t *resample_buf = malloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t));
  /* Device-format scratch for >16-bit sinks (e.g. 24-bit Bose); worst case is
   * MAX_RESAMPLE_FRAMES stereo frames at 4 bytes/sample. */
  uint8_t *conv_buf = malloc((size_t)MAX_RESAMPLE_FRAMES * 2 * 4);
  if (!pcm || !silence || !resample_buf || !conv_buf) {
    ESP_LOGE(TAG, "alloc failed");
    free(pcm);
    free(silence);
    free(resample_buf);
    free(conv_buf);
    vTaskDelete(NULL);
    return;
  }
  while (true) {
    if (resample_reinit_needed) {
      resample_reinit_needed = false;
      audio_resample_init((uint32_t)source_rate, s_out_rate, 2);
    }
    if (flush_requested) {
      flush_requested = false;
      audio_resample_reset();
      fifo_reset();
      fifo_prefill_silence();
    }
    /* No FIFO-level polling here: fifo_push() blocks on the stream buffer until
     * the iso callbacks free space, which paces this loop to the device. */
    size_t samples = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    if (samples > 0) {
      if (!s_streaming) {
        /* Standby: no USB device attached but the AirPlay session is live.
         * Discard the paced frames BEFORE the resampler/volume chain — the
         * receiver's timing gate paces this loop, so standby costs almost
         * nothing, and playback resumes the moment a device connects
         * (setup_device rebuilds the resampler and prefills the FIFO). */
        taskYIELD();
        continue;
      }
      int16_t *play_buf = pcm;
      size_t play_samples = samples;
      if (audio_resample_is_active()) {
        play_samples = audio_resample_process(pcm, samples, resample_buf,
                                              MAX_RESAMPLE_FRAMES);
        play_buf = resample_buf;
      }
      /* LED VU must track the SOURCE level, not the post-volume chain:
       * v1.1's independent two-stage attenuation (phone x device) scales the
       * PCM heavily, which drove the VU into silence at normal listening
       * volumes (light stayed off while playing, blue while paused). Feed it
       * BEFORE apply_volume(). */
      led_audio_feed(play_buf, play_samples);
      apply_volume(play_buf, play_samples * 2);
      apply_channel_mode(play_buf, play_samples);
      if (s_streaming) {
        if (s_frame_bytes == 4) {
          fifo_push((const uint8_t *)play_buf, play_samples * 4);
        } else {
          int n = expand_pcm(play_buf, (int)play_samples * 2, conv_buf,
                             s_out_subslot);
          fifo_push(conv_buf, (size_t)n);
        }
      } else {
        taskYIELD();
      }
    } else {
      led_audio_feed(silence, FRAME_SAMPLES);
      vTaskDelay(1);
    }
  }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t audio_output_init(void) {
  /* Called twice: early in app_main (USB up before the network waits, so a
   * noisy idle DAC gets silence ASAP) and from start_airplay_services(). */
  static bool s_inited = false;
  if (s_inited)
    return ESP_OK;
  s_inited = true;
  ESP_LOGI(TAG, "Init USB UAC HOST (native) output, rate=%d", OUTPUT_RATE);
  /* Realtime sessions re-anchor every ~1-2 s and the timing layer logs each
   * one at INFO ("Anchor set: ...") — steady chatter during normal play.
   * Quiet that TAG to warnings-and-up here instead of editing the shared
   * timing code; real problems (late drops, stuck anchors) are W-level and
   * still show. Comment out when debugging timing. */
  esp_log_level_set("audio_time", ESP_LOG_WARN);
  s_pcm = xStreamBufferCreate(FIFO_TARGET_BYTES, 1);
  s_ctrl_sem = xSemaphoreCreateBinary();
  if (!s_pcm || !s_ctrl_sem)
    return ESP_ERR_NO_MEM;

  /* Task that turns headset HID button presses into DACP commands (off the USB
   * callback, since DACP does blocking mDNS + HTTP). */
  s_hid_action_q = xQueueCreate(8, sizeof(int));
  if (s_hid_action_q)
    task_create_spiram(hid_action_task, "hid_act", 4096, NULL, 5, NULL, NULL);

  usb_host_vbus_enable();
  const usb_host_config_t host_cfg = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
      /* Carve the FS controller's 200-line (4 B each) FIFO for iso-OUT audio.
       * The Kconfig default (balanced bias) gives the periodic TX FIFO only 32
       * lines = 128 B max packet, so ANY real UAC speaker EP (192 B at
       * 48k/16-bit stereo, 384 B on high-rate alts) fails to allocate:
       * "HCD DWC: EP MPS (192) exceeds supported limit (128)". PTX 150 lines
       * = 600 B iso OUT; RX 34 lines = 128 B IN (HID buttons use 64); NPTX 16
       * lines = 64 B, enough for FS control writes. Same split as
       * CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT, but independent of the
       * builder's sdkconfig. */
      .fifo_settings_custom =
          {
              .nptx_fifo_lines = 16,
              .ptx_fifo_lines = 150,
              .rx_fifo_lines = 34,
          },
#endif
  };
  ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
  esp_register_shutdown_handler(usb_audio_shutdown);

  xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, USB_LIB_PRIO,
                          NULL, USB_CORE);
  xTaskCreatePinnedToCore(usb_client_task, "usb_cli", 5120, NULL, USB_CLI_PRIO,
                          NULL, USB_CORE);
  xTaskCreatePinnedToCore(usb_setup_task, "usb_setup", 5120, NULL, USB_CLI_PRIO,
                          NULL, USB_CORE);

  audio_resample_init(44100, OUTPUT_RATE, 2);
  return ESP_OK;
}

void audio_output_start(void) {
  /* Load the persisted device volume (settings_init() has already run at this
   * point). This is the independent "speaker" gain the web slider controls;
   * once a USB card attaches, setup_device() writes it into the card's own
   * volume (FU). The playback task multiplies it with the AirPlay/source
   * volume, which the phone controls and which never writes back here. */
  float db = -15.0f;
  if (settings_get_volume(&db) != ESP_OK) {
    db = -15.0f;
  }
  audio_output_set_device_volume_db(db);

  xTaskCreatePinnedToCore(playback_task, "uac_play", 4096, NULL, PLAYBACK_PRIO,
                          NULL, PLAYBACK_CORE);
}

void audio_output_flush(void) {
  flush_requested = true;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != source_rate) {
    source_rate = rate;
    resample_reinit_needed = true;
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  /* The real output buffering the AirPlay timing layer must lead by. The
   * playback task keeps the PCM FIFO near FIFO_TARGET_BYTES, and the iso URB
   * queue holds NUM_URBS x PACKETS_PER_URB ms in flight, so a frame pushed now
   * is not heard until it drains through both. The task pulls frames this far
   * ahead of their acoustic time to keep the pipeline full; if the timing layer
   * thinks the pipeline is only ~4 ms deep (the old hardcoded value), it treats
   * those pulled-ahead frames as "early" and emits silence, the receive buffer
   * backs up and ages, and stale frames then drop as "late" — the oscillation
   * heard as stutter. Reporting the true depth makes them release on time. */
  uint32_t fifo_us = (uint32_t)((uint64_t)s_fifo_target * 1000000ULL /
                                (s_out_rate * (uint32_t)s_frame_bytes));
  uint32_t urb_us = (uint32_t)NUM_URBS * PACKETS_PER_URB * 1000;
  return fifo_us + urb_us;
}

/* Live status for the web UI "音频编码状态 / Audio Encoding Status" module:
 * the encoding the connected USB sound card negotiated + stream state. */
bool audio_output_get_usb_audio_status(usb_audio_status_t *st) {
  if (!st) {
    return false;
  }
  memset(st, 0, sizeof(*st));
  st->attached = (s_dev != NULL);
  st->sample_rate = s_out_rate;
  st->bits = (uint8_t)(s_out_subslot * 8); /* subslot bytes -> bits */
  st->rate_readback = s_rate_readback;      /* EP GET_CUR after SET (0=unread) */
  st->channels = 2;
  st->uac_version = s_uac_ver;
  st->streaming = s_streaming;
  st->underruns = s_underruns;
  st->fifo_bytes = (uint32_t)fifo_level();
  st->fifo_cap = (uint32_t)s_fifo_target;
  st->hid_active = s_hid_active;
  st->hid_events = s_hid_events;
  st->hid_volup = s_hid_volup;
  st->hid_voldown = s_hid_voldown;
  st->hid_last_n = s_hid_last_n;
  for (int i = 0; i < 4; i++) {
    st->hid_last_raw[i] = s_hid_last_raw[i];
  }
  st->hid_press_n = s_hid_press_n;
  for (int i = 0; i < 4; i++) {
    st->hid_press_raw[i] = s_hid_press_raw[i];
  }
  st->fu_ch1_db = s_fu_ch1_db;
  st->fu_ch2_db = s_fu_ch2_db;
  snprintf(st->product, sizeof(st->product), "%s", s_product);
  st->alts_n = (uint8_t)(s_alt_count > 12 ? 12 : s_alt_count);
  for (int i = 0; i < st->alts_n; i++) {
    st->alts[i] = s_alt_info[i];
  }
  st->setup_stage = (int)s_setup_stage;
  st->setup_err = (int)s_setup_err;
  return st->attached;
}

/* v1.1: re-enumerate the attached card with the current user-chosen format.
 * Tears the device down and re-latches the NEW_DEV path, so the setup task
 * re-runs setup_device() (fresh descriptor parse + alt pick + sampling rate)
 * — a format change applies without unplugging or rebooting. Returns false
 * if no card is attached to re-enumerate. */
bool audio_output_usb_host_reprobe(void) {
  if (!s_dev) {
    return false;
  }
  uint8_t addr = s_dev_addr;
  teardown_device();
  /* The setup task acts on the latch once s_dev is free (teardown above runs
   * in the same iteration), exactly like a hot re-plug. */
  s_pending_addr = addr;
  s_connect_pending = true;
  ESP_LOGI(TAG, "Reprobe requested: re-enumerating device addr=%u", addr);
  return true;
}
