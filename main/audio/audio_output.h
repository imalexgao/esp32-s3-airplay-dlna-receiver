#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"

/**
 * Priority of the playback task in every output backend.
 *
 * It MUST outrank every audio source task (realtime UDP receiver = 8,
 * control receiver = 7, buffered TCP reader = 5) because the source tasks
 * are pinned to the same core.  A source task that outranks playback starves
 * it during a receive burst; the DMA ring (~46 ms) then runs dry and
 * auto_clear emits silence, so wall-clock advances while no audio is
 * consumed and the playout position slips permanently late.  That was the
 * mechanism behind the realtime-stream drift in issue #122 — the buffered
 * path was unaffected only because its reader task sits at priority 5.
 *
 * Playback cannot starve the sources in return: it blocks on the DMA write
 * for all but a few hundred microseconds of each ~8 ms frame period.
 */
#define AUDIO_PLAYBACK_TASK_PRIORITY 9

/**
 * Output channel mode. LEFT/RIGHT route the chosen source channel to both
 * speakers; MONO plays the (L+R)/2 downmix on both speakers; STEREO (default)
 * plays the normal left/right mix.
 */
typedef enum {
  AUDIO_CHANNEL_STEREO = 0,
  AUDIO_CHANNEL_LEFT,
  AUDIO_CHANNEL_RIGHT,
  AUDIO_CHANNEL_MONO,
} audio_channel_mode_t;

/**
 * Initialize the audio output backend (I2S / SPDIF / USB UAC).
 */
esp_err_t audio_output_init(void);

/**
 * Start the audio playback task.
 */
void audio_output_start(void);

/**
 * Flush output buffers (clears stale audio on pause/seek).
 */
void audio_output_flush(void);

/**
 * Stop the AirPlay playback task (for yielding I2S to another source)
 */
void audio_output_stop(void);

/**
 * Write raw PCM data to the I2S output.
 * Can be used by any audio source (BT A2DP, etc.) when the AirPlay
 * playback task is stopped.
 *
 * @param data   PCM data buffer (interleaved stereo, 16-bit)
 * @param bytes  Number of bytes to write
 * @param wait   Maximum ticks to wait for I2S DMA space
 * @return ESP_OK on success
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/**
 * Change the I2S sample rate (e.g. when BT negotiates 48 kHz)
 *
 * @param rate  Sample rate in Hz (e.g. 44100, 48000)
 */
void audio_output_set_sample_rate(uint32_t rate);

/**
 * Notify the output of the source sample rate (from AirPlay ANNOUNCE).
 * The resampler is re-initialized if the rate changes.
 */
void audio_output_set_source_rate(int rate);

/**
 * Return the I2S DMA pipeline latency in microseconds.
 *
 * This is computed from the DMA descriptor count and frame count
 * (both set at init time) divided by the output sample rate — i.e.
 *   (dma_desc_num × dma_frame_num × 1 000 000) / sample_rate
 *
 * Using this value instead of a hard-coded constant means the latency
 * stays correct if the DMA config or sample rate is ever changed.
 */
uint32_t audio_output_get_hardware_latency_us(void);

/**
 * Sample the live output pipeline delay: how long from now until the first
 * sample of the NEXT backend write is heard.
 *
 * Unlike audio_output_get_hardware_latency_us(), which models a permanently
 * half-full DMA ring, this reports the measured queue depth (frames handed
 * to the hardware minus frames the hardware reports as clocked out).  The
 * measurement is what makes the timing engine's error signal honest:
 *
 *   - it is unaffected by when the playback task happens to be scheduled,
 *     removing the one-sided "late read" noise the model suffers from;
 *   - after a writer stall it correctly reports a near-empty ring, so the
 *     engine sees the real lateness of the content it is about to submit
 *     instead of a fixed 43 ms guess.
 *
 * @param now_us      out: esp_timer_get_time() sampled with the queue depth.
 * @param pipeline_us out: queue depth in microseconds at the output rate.
 * @return false if the backend cannot report a hardware completion cursor,
 *         in which case the caller should fall back to the modelled latency.
 */
bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us);

/**
 * Local monotonic time (ns) at which the first sample of the NEXT backend
 * write will be heard.
 *
 * Deliberately built on the stable modelled latency rather than the live
 * cursor from audio_output_get_pipeline_us(): this is a *scheduling* input,
 * consumed once per render to pick which RTP timestamp to emit next.  The
 * live cursor steps at DMA descriptor boundaries, so feeding it in here would
 * make the chosen timestamp jitter by a whole descriptor and repeatedly
 * re-target the read cursor.  Error measurement still uses the live value.
 */
int64_t audio_output_get_next_playout_time_ns(int64_t now_us);

/**
 * Number of output-underrun episodes since boot: the DMA clocked out
 * descriptors the playback task never filled, so that much output time was
 * emitted as silence and lost from the playout position.  Non-zero values
 * mean the playback task is being starved.
 */
uint32_t audio_output_get_underruns(void);

/**
 * Cycle the output channel mode: STEREO -> LEFT -> RIGHT -> MONO -> STEREO.
 * The new mode is persisted to NVS.
 * @return the new mode after cycling.
 */
audio_channel_mode_t audio_output_cycle_channel_mode(void);

/**
 * Set the output channel mode directly and persist it to NVS.
 */
void audio_output_set_channel_mode(audio_channel_mode_t mode);

/**
 * Get the current output channel mode.
 */
audio_channel_mode_t audio_output_get_channel_mode(void);

/**
 * True when the DAC configuration already fixes the per-output routing, in
 * which case the mode is forced to STEREO and set/cycle are ignored.
 */
bool audio_output_channel_mode_locked(void);

/**
 * True when a DSP flow makes the channel selection instead of the software
 * downmix. The outputs are then crossover ways rather than left and right, so
 * STEREO means the (L+R)/2 mix and only LEFT and RIGHT pick a single channel.
 */
bool audio_output_channel_mode_in_dsp(void);

#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
/** One audio-streaming altsetting discovered during enumeration. */
typedef struct {
  uint32_t rate;    /* Hz the alt would run (resolved; 0 = unknown / UAC2) */
  uint32_t rates[3]; /* every Hz this alt lists (UAC1, up to 3) */
  uint8_t rates_n;  /* how many rates are valid in rates[] (>= 1) */
  uint8_t bits;     /* bits per sample (subslot bytes * 8) */
  uint8_t subslot;  /* bytes per sample (2/3/4) */
  uint16_t mps;     /* wMaxPacketSize of its endpoint */
  uint8_t sync;     /* endpoint sync type: 0 none, 1 async, 2 adaptive, 3 sync */
  uint8_t out;      /* 1 = iso OUT (host->device), 0 = iso IN (device->host) */
  uint8_t channels; /* channels */
  uint8_t iface;    /* interface number */
  uint8_t alt;      /* altsetting number */
} usb_alt_info_t;

/**
 * Live status of the USB Audio Class HOST output — the encoding the attached
 * USB sound card / DAC negotiated, plus stream state. Feeds the web UI's
 * "音频编码状态 / Audio Encoding Status" module.
 */
typedef struct {
  bool attached;        /* a USB audio device is enumerated */
  uint32_t sample_rate; /* negotiated output rate in Hz (e.g. 44100/48000) */
  uint32_t rate_readback; /* EP sampling-freq GET_CUR after SET (0=unread) */
  uint8_t bits;         /* bits per sample: 16 / 24 / 32 */
  uint8_t channels;     /* output channels (2 = stereo on this path) */
  uint8_t uac_version;  /* UAC spec major version: 1 or 2 */
  bool streaming;       /* iso-OUT stream currently running */
  uint32_t underruns;   /* FIFO underruns (audible gaps) since boot */
  uint32_t fifo_bytes;  /* current PCM FIFO fill in bytes */
  uint32_t fifo_cap;    /* PCM FIFO capacity in bytes */
  bool hid_active;      /* HID media-button interface claimed and reading */
  uint32_t hid_events;  /* HID key presses received since attach */
  uint32_t hid_volup;   /* parsed VOL_UP presses */
  uint32_t hid_voldown; /* parsed VOL_DOWN presses */
  uint8_t hid_last_raw[4]; /* last HID report bytes (mapping diagnostics) */
  uint8_t hid_last_n;      /* valid bytes in hid_last_raw */
  uint8_t hid_press_raw[4]; /* raw bytes at a PRESS edge (vol- decode) */
  uint8_t hid_press_n;      /* valid bytes in hid_press_raw */
  float fu_ch1_db;     /* FU volume ch1 (L) readback after 0dB write */
  float fu_ch2_db;     /* FU volume ch2 (R) readback after 0dB write */
  char product[64];     /* USB device iProduct string descriptor */
  uint8_t alts_n;       /* number of discovered audio altsettings (<= 12) */
  usb_alt_info_t alts[12]; /* every PCM alt the device offers (menu) */
  int setup_stage;      /* attach attempt stopped at (0 none ... 9 done) */
  int setup_err;        /* last setup failure's esp_err (0 = no failure) */
} usb_audio_status_t;

/**
 * One Feature Unit of the attached card. Several cards (e.g. KEF EGG) expose
 * more than one FU; the one that actually sits in the speaker's audible path
 * is not always the one a topology guess picks, so the web UI lists every FU
 * with its live volume — that is how a "volume writes OK but sound doesn't
 * change" case is diagnosed.
 */
typedef struct {
  uint8_t id;        /* FU unit id */
  uint8_t src;       /* bSourceID (upstream entity) */
  bool is_spk;       /* matched as speaker-path FU */
  bool is_mic;       /* feeds the USB mic-out */
  float default_db;  /* volume before any of our writes (factory state) */
  float cur_db;      /* live readback */
  bool read_ok;      /* GET_CUR currently succeeding */
} fu_info_t;

/** Copy the FU table (0..7 entries) into @p out; returns the count. */
int audio_output_get_fu_info(fu_info_t *out, int max);

/** Copy the discovered altsetting menu (<= 12) into @p out; returns count. */
int audio_output_get_alt_info(usb_alt_info_t *out, int max);
/**
 * Fill @p st with the current USB audio host status.
 * @return true when a device is attached (st is then meaningful).
 */
bool audio_output_get_usb_audio_status(usb_audio_status_t *st);

/**
 * Re-enumerate the attached sound card with the current user-chosen output
 * format (rate x bits) — applies a web format change without unplugging or
 * rebooting. No-op when no card is attached.
 * @return true if a re-enumeration was requested.
 */
bool audio_output_usb_host_reprobe(void);

/**
 * Set the independent DEVICE volume (dB, -30..0) applied at the USB output
 * stage on top of the AirPlay/source volume. This is the volume the web
 * slider controls — it is NOT linked to the phone's volume. When a USB sound
 * card with a speaker Feature Unit is attached, this writes the CARD's own
 * volume (the same stage the card's remote control adjusts).
 */
void audio_output_set_device_volume_db(float volume_db);

/**
 * Get the current device volume in dB (-30..0). With a speaker FU attached
 * this is the card's live volume (last SET/GET_CUR); otherwise the software
 * gain setting.
 */
float audio_output_get_device_volume_db(void);

/**
 * Read the card's current speaker-FU volume (GET_CUR) into the value returned
 * by audio_output_get_device_volume_db(). Called from the web status poll so
 * a remote-control change appears in the web slider; the change is also
 * persisted so the boot-time FU write matches. No-op without a speaker FU.
 */
void audio_output_refresh_fu_volume(void);

/** True when a USB speaker FU is attached (device volume = card's volume). */
bool audio_output_has_fu_volume(void);

/** FU volume range (dB), as read from the card. 0.0 = unknown/unavailable. */
float audio_output_get_fu_min_db(void);
float audio_output_get_fu_max_db(void);
uint16_t audio_output_get_config_raw(const uint8_t **buf); /* /api/desc dump */
float audio_output_get_fu_vol_db(void);

/**
 * One-shot diagnostic of the attached card's speaker Feature Unit: whether
 * SET_CUR / GET_CUR actually work and whether the volume range extends above
 * 0 dB. Runs at every card setup; results are shown in the web UI so the
 * "why is it still quiet / why is the remote dead" questions can be answered
 * without a serial connection.
 */
typedef struct {
  bool probe_ran;       /* true after the card's setup probed its speaker FU */
  bool range_ok;        /* GET_MIN/GET_MAX succeeded */
  float min_db, max_db; /* FU volume range (0/0 when unknown) */
  bool set0_ok;         /* SET 0 dB accepted */
  bool get0_ok;         /* GET_CUR right after SET 0 dB returned a value */
  float get0_db;        /* value read back after SET 0 dB */
  bool set6_ok;         /* SET +6 dB accepted */
  bool get6_ok;         /* GET_CUR after SET +6 dB returned a value */
  float get6_db;        /* value read back after SET +6 dB */
  float max_probe_db;   /* highest volume the card actually held (its ceiling) */
  uint32_t get_fail_count; /* runtime GET_CUR failures since attach */
  bool get_ever_ok;        /* any runtime GET_CUR succeeded */
} fu_volume_probe_t;

/** Copy the last FU probe results into @p out. */
void audio_output_get_fu_probe(fu_volume_probe_t *out);

/**
 * Peak output level since the last call, in dBFS (0 = full scale, -99 = no
 * audio). Reading resets the meter. Useful to tell whether the digital path
 * is at full scale (quiet playback is then caused by the speaker itself).
 */
float audio_output_get_peak_dbfs(void);
#endif /* CONFIG_AUDIO_OUTPUT_USB_HOST */
