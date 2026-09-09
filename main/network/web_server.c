#include "web_server.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_wifi.h"

#include "playback_control.h"
#include "settings.h"
#include "led.h"
#include "wifi.h"
#include "ethernet.h"
#include "ota.h"
#include "log_stream.h"
#include "rtsp_server.h"
#include "rtsp_events.h"
#include "audio_output.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_DAC_TAS58XX
#include "dac_tas58xx.h"
#endif

#ifdef CONFIG_DAC_TAS57XX
#include "dac_tas57xx.h"
#endif

/* Sub level-trim (2.1 subwoofer) is a TAS57xx concept: that driver flags one
 * device as the sub and offsets it from the master volume. The TAS58xx driver
 * trims every amplifier independently instead, through /api/bq. */
#if defined(CONFIG_DAC_TAS57XX)
#define DAC_HAS_SUB_OFFSET       1
#define DAC_SUB_OFFSET_MIN_DB    TAS57XX_SUB_OFFSET_MIN_DB
#define DAC_SUB_OFFSET_MAX_DB    TAS57XX_SUB_OFFSET_MAX_DB
#define dac_get_sub_offset_db()  dac_tas57xx_get_sub_offset_db()
#define dac_set_sub_offset_db(x) dac_tas57xx_set_sub_offset_db(x)
/* The trim only moves devices flagged is_sub, which is index > 0, so a
 * single-amplifier board has nothing for it to act on. */
#define dac_has_sub() (dac_tas57xx_get_device_count() > 1)
/* Per-channel level and mute, which only the TAS57xx driver implements. */
#define DAC_HAS_CH_TRIM    1
#define DAC_CH_TRIM_MIN_DB TAS57XX_CH_TRIM_MIN_DB
#define DAC_CH_TRIM_MAX_DB TAS57XX_CH_TRIM_MAX_DB
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define SPIFFS_CHUNK_SIZE 1024

static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *path,
                                   const char *content_type) {
  FILE *f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s", path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, content_type);
  char buf[SPIFFS_CHUNK_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// API handlers
static esp_err_t root_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/index.html", "text/html");
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t logs_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/logs.html", "text/html");
}

static esp_err_t speedtest_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/speedtest.html", "text/html");
}

// Tiny endpoint used by JS for RTT timing. Returns minimal body.
static esp_err_t speedtest_ping_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, "ok", 2);
  return ESP_OK;
}

// Streams `bytes` octets of filler data so the browser can measure DL speed.
// Capped to avoid pathological requests starving audio.
#define SPEEDTEST_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define SPEEDTEST_CHUNK     2048

static esp_err_t speedtest_download_handler(httpd_req_t *req) {
  size_t bytes = (size_t)1024 * 1024;
  char qbuf[64];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(qbuf, "bytes", val, sizeof(val)) == ESP_OK) {
      long v = strtol(val, NULL, 10);
      if (v > 0) {
        bytes = (size_t)v;
      }
    }
  }
  if (bytes > SPEEDTEST_MAX_BYTES) {
    bytes = SPEEDTEST_MAX_BYTES;
  }

  // Reuse a single buffer of filler bytes. Static so we don't repeatedly
  // hammer the heap; content is irrelevant but non-zero to thwart any
  // compression along the way.
  static uint8_t filler[SPEEDTEST_CHUNK];
  static bool filler_init = false;
  if (!filler_init) {
    for (size_t i = 0; i < sizeof(filler); i++) {
      filler[i] = (uint8_t)(i * 37);
    }
    filler_init = true;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  size_t remaining = bytes;
  while (remaining > 0) {
    ssize_t n =
        remaining < SPEEDTEST_CHUNK ? (ssize_t)remaining : SPEEDTEST_CHUNK;
    if (httpd_resp_send_chunk(req, (const char *)filler, n) != ESP_OK) {
      return ESP_FAIL;
    }
    remaining -= (size_t)n;
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// Consumes a POST body and reports how many bytes were received.
static esp_err_t speedtest_upload_handler(httpd_req_t *req) {
  size_t total = req->content_len;
  size_t got = 0;
  uint8_t buf[SPEEDTEST_CHUNK];
  while (got < total) {
    size_t want = total - got;
    if (want > sizeof(buf)) {
      want = sizeof(buf);
    }
    int r = httpd_req_recv(req, (char *)buf, want);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    got += (size_t)r;
  }
  char reply[64];
  int n = snprintf(reply, sizeof(reply), "received=%u", (unsigned)got);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, reply, n);
  return ESP_OK;
}

// Captive portal detection handlers
// These endpoints are requested by various OS to detect captive portals
static esp_err_t captive_portal_redirect(httpd_req_t *req) {
  // Redirect to the configuration page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// Apple devices (iOS/macOS) check these
static esp_err_t captive_apple_handler(httpd_req_t *req) {
  // Apple expects specific response, redirect instead
  return captive_portal_redirect(req);
}

// Android checks this
static esp_err_t captive_android_handler(httpd_req_t *req) {
  // Android expects 204 for no captive portal, anything else triggers portal
  return captive_portal_redirect(req);
}

// Windows checks this
static esp_err_t captive_windows_handler(httpd_req_t *req) {
  return captive_portal_redirect(req);
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;

  cJSON *json = cJSON_CreateObject();
  esp_err_t err = wifi_scan(&ap_list, &ap_count);

  if (err == ESP_OK && ap_list) {
    cJSON *networks = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_count; i++) {
      cJSON *net = cJSON_CreateObject();
      cJSON_AddStringToObject(net, "ssid", (char *)ap_list[i].ssid);
      cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
      cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
      cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddItemToObject(json, "networks", networks);
    cJSON_AddBoolToObject(json, "success", true);
    free(ap_list);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
  }

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  return ESP_OK;
}

static esp_err_t wifi_config_handler(httpd_req_t *req) {
  char content[512];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *ssid_json = cJSON_GetObjectItem(json, "ssid");
  cJSON *password_json = cJSON_GetObjectItem(json, "password");

  cJSON *response = cJSON_CreateObject();
  if (ssid_json && cJSON_IsString(ssid_json)) {
    const char *ssid = cJSON_GetStringValue(ssid_json);
    const char *password = password_json && cJSON_IsString(password_json)
                               ? cJSON_GetStringValue(password_json)
                               : "";

    esp_err_t err = settings_set_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
      ESP_LOGI(TAG, "WiFi credentials saved. We are restarting...");
      // Schedule restart
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid SSID");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

static esp_err_t device_name_handler(httpd_req_t *req) {
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *name_json = cJSON_GetObjectItem(json, "name");
  cJSON *response = cJSON_CreateObject();

  if (name_json && cJSON_IsString(name_json)) {
    const char *name = cJSON_GetStringValue(name_json);
    esp_err_t err = settings_set_device_name(name);
    if (err == ESP_OK) {
      wifi_set_hostname(name);
      ethernet_set_hostname(name);
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid name");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

/* Copy a JSON string field into a fixed metadata slot. Absent or non-string
 * fields are left zeroed, which listeners treat as "unchanged". */
static void metadata_copy_field(const cJSON *json, const char *key, char *dst) {
  const cJSON *item = cJSON_GetObjectItem(json, key);
  if (cJSON_IsString(item) && item->valuestring != NULL) {
    snprintf(dst, METADATA_STRING_MAX, "%s", item->valuestring);
  }
}

static uint32_t metadata_uint_field(const cJSON *json, const char *key) {
  const cJSON *item = cJSON_GetObjectItem(json, key);
  if (cJSON_IsNumber(item) && item->valuedouble > 0) {
    return (uint32_t)item->valuedouble;
  }
  return 0;
}

/* Push now-playing info from an external source, e.g. a host-side helper
 * feeding metadata for USB audio, which UAC itself cannot carry. */
static esp_err_t metadata_post_handler(httpd_req_t *req) {
  char content[512];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  rtsp_event_data_t data = {0};
  metadata_copy_field(json, "title", data.metadata.title);
  metadata_copy_field(json, "artist", data.metadata.artist);
  metadata_copy_field(json, "album", data.metadata.album);
  metadata_copy_field(json, "genre", data.metadata.genre);
  data.metadata.duration_secs = metadata_uint_field(json, "duration");
  data.metadata.position_secs = metadata_uint_field(json, "position");
  cJSON_Delete(json);

  rtsp_events_emit(RTSP_EVENT_METADATA, &data);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t led_brightness_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "brightness", led_get_brightness());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t led_brightness_post_handler(httpd_req_t *req) {
  char content[64];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "brightness");
  if (val && cJSON_IsNumber(val)) {
    int b = (int)val->valuedouble;
    if (b < 0) {
      b = 0;
    }
    if (b > 255) {
      b = 255;
    }
    esp_err_t err = led_set_brightness((uint8_t)b);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected {\"brightness\": 0-255}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* Read a whole request body into a NUL-terminated heap buffer. httpd_req_recv()
 * can return a short read, so keep going until the declared length arrives. */
static char *recv_body(httpd_req_t *req, size_t max_len) {
  int len = req->content_len;
  if (len <= 0 || (size_t)len > max_len) {
    return NULL;
  }
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    return NULL;
  }
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, buf + got, (size_t)(len - got));
    if (r <= 0) {
      free(buf);
      return NULL;
    }
    got += r;
  }
  buf[len] = '\0';
  return buf;
}

/* True for a JSON number that is a whole value inside [lo, hi]. */
static bool json_int_in_range(const cJSON *v, int lo, int hi) {
  return cJSON_IsNumber(v) && v->valuedouble == (double)v->valueint &&
         v->valueint >= lo && v->valueint <= hi;
}

static esp_err_t channel_mode_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "mode", audio_output_get_channel_mode());
  cJSON_AddBoolToObject(json, "locked", audio_output_channel_mode_locked());
  cJSON_AddBoolToObject(json, "dsp", audio_output_channel_mode_in_dsp());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t channel_mode_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "mode");
  if (json_int_in_range(val, AUDIO_CHANNEL_STEREO, AUDIO_CHANNEL_MONO)) {
    audio_output_set_channel_mode((audio_channel_mode_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", true);
    /* Boards with two amplifiers keep stereo whatever was asked for. */
    cJSON_AddNumberToObject(response, "mode", audio_output_get_channel_mode());
    cJSON_AddBoolToObject(response, "locked",
                          audio_output_channel_mode_locked());
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"mode\": 0-3}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* AirPlay dB scale, matching playback_control's clamp. */
#define VOLUME_UI_MIN_DB -30.0f
#define VOLUME_UI_MAX_DB 0.0f

static esp_err_t volume_get_handler(httpd_req_t *req) {
  float db = -15.0f;
#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
  /* The DEVICE volume: a digital gain (0 dB = full-scale pass-through, the
   * same signal Windows sends at 100%). It is independent of the phone's
   * AirPlay volume and never uses the card's FU (KEF EGG ignores FU writes
   * while streaming). */
  db = audio_output_get_device_volume_db();
#else
  settings_get_volume(&db);
#endif
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "volume_db", db);
  cJSON_AddNumberToObject(json, "min", VOLUME_UI_MIN_DB);
  cJSON_AddNumberToObject(json, "max", VOLUME_UI_MAX_DB);
  cJSON_AddBoolToObject(json, "muted", playback_control_is_muted());
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "uptime_s",
                          (double)(esp_timer_get_time() / 1000000));
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t volume_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 512);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *json = cJSON_Parse(content);
  free(content);
  cJSON *val = json ? cJSON_GetObjectItem(json, "volume_db") : NULL;
  if (!cJSON_IsNumber(val)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected volume_db");
    return ESP_FAIL;
  }
  float db = (float)val->valuedouble;
  cJSON_Delete(json);
  if (db < VOLUME_UI_MIN_DB) {
    db = VOLUME_UI_MIN_DB;
  }
  if (db > VOLUME_UI_MAX_DB) {
    db = VOLUME_UI_MAX_DB;
  }
  /* Apply to the live session and persist to NVS (same path as the hardware
   * volume buttons). The phone's volume is a separate stage (AirPlay session
   * volume) and is not synced. */
  playback_control_set_volume(db);

  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "success", true);
  cJSON_AddNumberToObject(response, "volume_db", db);
  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(response);
  return ESP_OK;
}

#ifdef DAC_HAS_SUB_OFFSET
static esp_err_t sub_offset_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "offset", dac_get_sub_offset_db());
  cJSON_AddNumberToObject(json, "min", DAC_SUB_OFFSET_MIN_DB);
  cJSON_AddNumberToObject(json, "max", DAC_SUB_OFFSET_MAX_DB);
  cJSON_AddBoolToObject(json, "available", dac_has_sub());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t sub_offset_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 2048);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "offset");
  if (val && cJSON_IsNumber(val)) {
    float off = (float)val->valuedouble;
    if (off < DAC_SUB_OFFSET_MIN_DB) {
      off = DAC_SUB_OFFSET_MIN_DB;
    }
    if (off > DAC_SUB_OFFSET_MAX_DB) {
      off = DAC_SUB_OFFSET_MAX_DB;
    }
    dac_set_sub_offset_db(off);
    if (settings_set_sub_offset(off) == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", "Applied but could not save");
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"offset\": dB}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}
#endif /* DAC_HAS_SUB_OFFSET */

#ifdef DAC_HAS_CH_TRIM
static void ch_trim_add_state(cJSON *json) {
  cJSON *arr = cJSON_AddArrayToObject(json, "channels");
  for (int ch = 0; ch < TAS57XX_CHANNELS; ch++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "trim", dac_tas57xx_get_channel_trim_db(ch));
    cJSON_AddBoolToObject(o, "mute", dac_tas57xx_get_channel_mute(ch));
    cJSON_AddItemToArray(arr, o);
  }
  cJSON_AddNumberToObject(json, "min", DAC_CH_TRIM_MIN_DB);
  cJSON_AddNumberToObject(json, "max", DAC_CH_TRIM_MAX_DB);
}

static esp_err_t ch_trim_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  ch_trim_add_state(json);
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t ch_trim_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 512);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  cJSON *arr = json ? cJSON_GetObjectItem(json, "channels") : NULL;
  if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != TAS57XX_CHANNELS) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Expected {\"channels\":[{\"trim\":dB,\"mute\":bool},"
                        "{...}]}");
    return ESP_FAIL;
  }

  /* Mute is a listening aid rather than a setting, so only the trims are
   * written back to NVS. */
  bool trim_changed = false;
  for (int ch = 0; ch < TAS57XX_CHANNELS; ch++) {
    cJSON *o = cJSON_GetArrayItem(arr, ch);
    cJSON *trim = cJSON_GetObjectItem(o, "trim");
    cJSON *mute = cJSON_GetObjectItem(o, "mute");
    if (cJSON_IsNumber(trim)) {
      dac_tas57xx_set_channel_trim_db(ch, (float)trim->valuedouble);
      trim_changed = true;
    }
    if (cJSON_IsBool(mute)) {
      dac_tas57xx_set_channel_mute(ch, cJSON_IsTrue(mute));
    }
  }
  cJSON_Delete(json);

  esp_err_t save_err = ESP_OK;
  if (trim_changed) {
    float saved[TAS57XX_CHANNELS];
    for (int ch = 0; ch < TAS57XX_CHANNELS; ch++) {
      saved[ch] = dac_tas57xx_get_channel_trim_db(ch);
    }
    save_err = settings_set_channel_trim(saved);
  }

  cJSON *response = cJSON_CreateObject();
  ch_trim_add_state(response);
  if (save_err == ESP_OK) {
    cJSON_AddBoolToObject(response, "success", true);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Applied but could not save");
  }
  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(response);
  return ESP_OK;
}
#endif /* DAC_HAS_CH_TRIM */

#ifdef CONFIG_DAC_TAS58XX
/* How the second amplifier on a dual-DAC board is wired: bridged (PBTL) mono
 * or a stereo pair. Any crossover between the two is a matter for the biquad
 * chains, so this is the whole of the dual-DAC configuration. */
static esp_err_t dual_mode_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "devices", dac_tas58xx_get_device_count());
  cJSON_AddBoolToObject(json, "pbtl", dac_tas58xx_get_second_pbtl());
  cJSON_AddBoolToObject(json, "restart_required",
                        dac_tas58xx_get_second_pbtl() !=
                            dac_tas58xx_get_active_second_pbtl());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t dual_mode_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "pbtl");
  if (!val || !cJSON_IsBool(val)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"pbtl\": bool}");
  } else {
    const bool pbtl = cJSON_IsTrue(val);
    dac_tas58xx_set_second_pbtl(pbtl);
    settings_set_second_pbtl(pbtl);
    cJSON_AddBoolToObject(response, "success", true);
    /* PBTL is a control-port setting that can only be changed while the
     * output stage is idle, so the change lands on the next boot. */
    cJSON_AddBoolToObject(response, "restart_required", true);
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}
#endif /* CONFIG_DAC_TAS58XX */

static esp_err_t airplay_mode_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "v1", settings_airplay_v1_configured());
  cJSON_AddNumberToObject(json, "port", airplay_rtsp_port());
  cJSON_AddBoolToObject(json, "restart_required",
                        settings_airplay_v1_configured() !=
                            settings_airplay_v1());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t airplay_mode_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "v1");
  if (!val || !cJSON_IsBool(val)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"v1\": bool}");
  } else {
    const bool v1 = cJSON_IsTrue(val);
    esp_err_t err = settings_set_airplay_v1(v1);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    } else {
      // The mDNS records and the RTSP listener are both built at startup.
      cJSON_AddBoolToObject(response, "restart_required",
                            v1 != settings_airplay_v1());
    }
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* 遥控布局 (remote HID layout): Apple standard (default) vs KEF EGG.
 * Unlike the AirPlay mode this applies immediately — the decoder reads the
 * setting live, so no restart is needed. */
static esp_err_t remote_layout_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON_AddBoolToObject(json, "egg", settings_remote_layout_egg());
  /* KEF 有源音箱专版把布局编译进固件：UI 隐藏选择器，POST 也一律强制 KEF。 */
  cJSON_AddBoolToObject(json, "locked", settings_remote_layout_locked());
  cJSON_AddBoolToObject(json, "success", true);
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
}

static esp_err_t remote_layout_post_handler(httpd_req_t *req) {
#ifdef CONFIG_REMOTE_LAYOUT_KEF_EDITION
  /* KEF 专版：布局锁死，任何请求都保持 KEF EGG。 */
  (void)req;
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "egg", true);
  cJSON_AddBoolToObject(json, "locked", true);
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
#else
  char *content = recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "egg");
  if (!val || !cJSON_IsBool(val)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected {\"egg\": bool}");
  } else {
    esp_err_t err = settings_set_remote_layout_egg(cJSON_IsTrue(val));
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }
  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
#endif
}

/* ── 输出格式 (v1.1)：采样率 x 位深 ─────────────────────────────────────── */
static esp_err_t audio_format_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  uint8_t fmt = settings_get_audio_fmt();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "fmt", fmt);
  cJSON_AddNumberToObject(json, "count", SETTINGS_AUDIO_FMT_COUNT);
  cJSON *opts = cJSON_AddArrayToObject(json, "options");
  for (int i = 0; i < SETTINGS_AUDIO_FMT_COUNT; i++) {
    uint32_t rate;
    uint8_t bits;
    settings_audio_fmt_params((uint8_t)i, &rate, &bits, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "fmt", i);
    cJSON_AddNumberToObject(o, "rate", rate);
    cJSON_AddNumberToObject(o, "bits", bits);
    cJSON_AddStringToObject(o, "label", settings_audio_fmt_label((uint8_t)i));
    cJSON_AddItemToArray(opts, o);
  }
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
}

static esp_err_t audio_format_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 64);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "fmt");
  if (!val || !cJSON_IsNumber(val) || val->valueint < 0 ||
      val->valueint >= SETTINGS_AUDIO_FMT_COUNT) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid format index");
  } else {
    esp_err_t err = settings_set_audio_fmt((uint8_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }
  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ── 界面语言 (v1.1)：中文 / English ───────────────────────────────────── */
static esp_err_t ui_lang_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "lang", settings_get_ui_lang());
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
}

static esp_err_t ui_lang_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 64);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "lang");
  if (!val || !cJSON_IsNumber(val) || (val->valueint != 0 && val->valueint != 1)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "lang must be 0 (zh) or 1 (en)");
  } else {
    esp_err_t err = settings_set_ui_lang((uint8_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }
  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ── 低延时模式 (v1.1)：180/60/20 ms 预滚 ───────────────────────────────── */
static esp_err_t latency_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  uint8_t mode = settings_get_latency_mode();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "mode", mode);
  static const int k_preroll[3] = {180, 60, 20};
  cJSON_AddNumberToObject(json, "preroll_ms",
                          k_preroll[mode <= 2 ? mode : 0]);
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
}

static esp_err_t latency_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 64);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "mode");
  if (!val || !cJSON_IsNumber(val) || val->valueint < 0 || val->valueint > 2) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "mode must be 0..2");
  } else {
    esp_err_t err = settings_set_latency_mode((uint8_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }
  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
/* 重新应用当前输出格式：重新枚举 USB 声卡（无需拔插）。 */
static esp_err_t usb_reprobe_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  bool ok = audio_output_usb_host_reprobe();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "reprobed", ok);
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
}
#endif

static esp_err_t ota_update_handler(httpd_req_t *req) {
  if (req->content_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware uploaded");
    return ESP_FAIL;
  }

  // Stop AirPlay to free resources during OTA
  ESP_LOGI(TAG, "Stopping AirPlay for OTA update");
  rtsp_server_stop();

  esp_err_t err = ota_start_from_http(req);

  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return ESP_FAIL;
  }

  // Send response before restarting
  httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

static const char *reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_POWERON:
    return "poweron";
  case ESP_RST_EXT:
    return "external";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "int_wdt";
  case ESP_RST_TASK_WDT:
    return "task_wdt";
  case ESP_RST_WDT:
    return "other_wdt";
  case ESP_RST_DEEPSLEEP:
    return "deepsleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  default:
    return "unknown";
  }
}

static esp_err_t system_info_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *info = cJSON_CreateObject();

  char ip_str[16] = {0};
  char mac_str[18] = {0};
  char device_name[65] = {0};
  bool wifi_connected = wifi_is_connected();
  bool eth_connected = ethernet_is_connected();

  // Show IP and MAC for the active interface
  if (eth_connected) {
    ethernet_get_ip_str(ip_str, sizeof(ip_str));
    ethernet_get_mac_str(mac_str, sizeof(mac_str));
  } else {
    wifi_get_ip_str(ip_str, sizeof(ip_str));
    wifi_get_mac_str(mac_str, sizeof(mac_str));
  }
  settings_get_device_name(device_name, sizeof(device_name));

  cJSON_AddStringToObject(info, "ip", ip_str);
  cJSON_AddStringToObject(info, "mac", mac_str);
  cJSON_AddStringToObject(info, "device_name", device_name);
  cJSON_AddBoolToObject(info, "wifi_connected", wifi_connected);
  cJSON_AddBoolToObject(info, "eth_connected", eth_connected);
  cJSON_AddNumberToObject(info, "free_heap", esp_get_free_heap_size());

  // WiFi link diagnostics (only meaningful when associated as STA)
  if (wifi_connected) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      char ssid_buf[33];
      size_t slen = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
      if (slen > sizeof(ssid_buf) - 1) {
        slen = sizeof(ssid_buf) - 1;
      }
      memcpy(ssid_buf, ap.ssid, slen);
      ssid_buf[slen] = '\0';
      char bssid_buf[18];
      snprintf(bssid_buf, sizeof(bssid_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
               ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
               ap.bssid[5]);
      const char *phy = "?";
      if (ap.phy_11n) {
        phy = "11n";
      } else if (ap.phy_11g) {
        phy = "11g";
      } else if (ap.phy_11b) {
        phy = "11b";
      } else if (ap.phy_lr) {
        phy = "LR";
      }
      cJSON_AddStringToObject(info, "wifi_ssid", ssid_buf);
      cJSON_AddStringToObject(info, "wifi_bssid", bssid_buf);
      cJSON_AddNumberToObject(info, "wifi_rssi", ap.rssi);
      cJSON_AddNumberToObject(info, "wifi_channel", ap.primary);
      cJSON_AddStringToObject(info, "wifi_phy", phy);
    }
  }
  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddStringToObject(info, "firmware_version", app_desc->version);
  cJSON_AddBoolToObject(info, "kef_edition",
#ifdef CONFIG_REMOTE_LAYOUT_KEF_EDITION
                        true
#else
                        false
#endif
  );
  cJSON_AddStringToObject(info, "reset_reason",
                          reset_reason_str(esp_reset_reason()));
  cJSON_AddNumberToObject(info, "uptime_s",
                          (double)(esp_timer_get_time() / 1000000));
#ifdef CONFIG_DAC_TAS58XX
  cJSON_AddBoolToObject(info, "eq_supported", true);
#else
  cJSON_AddBoolToObject(info, "eq_supported", false);
#endif
#ifdef DAC_HAS_SUB_OFFSET
  cJSON_AddBoolToObject(info, "sub_supported", dac_has_sub());
#else
  cJSON_AddBoolToObject(info, "sub_supported", false);
#endif
#ifdef CONFIG_DAC_TAS58XX
  cJSON_AddBoolToObject(info, "dual_supported", true);
#else
  cJSON_AddBoolToObject(info, "dual_supported", false);
#endif
#ifdef CONFIG_DAC_TAS57XX
  cJSON_AddBoolToObject(info, "hf1_supported", dac_tas57xx_hf1_available());
  cJSON_AddBoolToObject(info, "hf3_supported", dac_tas57xx_hf3_available());
#else
  cJSON_AddBoolToObject(info, "hf1_supported", false);
  cJSON_AddBoolToObject(info, "hf3_supported", false);
#endif

  cJSON_AddItemToObject(json, "info", info);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  return ESP_OK;
}

#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
/* 完整配置描述符 dump（诊断 EGG 静音路径用）— raw hex + 解析后的音频实体。 */
static esp_err_t usb_desc_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  const uint8_t *raw = NULL;
  uint16_t n = audio_output_get_config_raw(&raw);
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "len", n);
  if (n && raw) {
    char hex[1024];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(hex) - 2; i++) {
      o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x", raw[i]);
    }
    cJSON_AddStringToObject(json, "hex", hex);

    /* Walk every interface descriptor; parse the AudioControl (class 1)
     * entities so a missing gain control shows up at a glance. */
    cJSON *ifs = cJSON_AddArrayToObject(json, "interfaces");
    int i = 0;
    while (i + 2 <= n) {
      uint8_t len = raw[i], type = raw[i + 1];
      if (len < 2 || i + len > n)
        break;
      if (type == 0x04) { /* interface descriptor */
        uint8_t inum = raw[i + 2], alt = raw[i + 3];
        uint8_t cls = raw[i + 5], sub = raw[i + 6];
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "iface", inum);
        cJSON_AddNumberToObject(it, "alt", alt);
        cJSON_AddNumberToObject(it, "class", cls);
        cJSON_AddNumberToObject(it, "subclass", sub);
        cJSON_AddItemToArray(ifs, it);
        /* Parse the AC interface (subclass 1) class-specific descriptors. */
        if (cls == 1 && sub == 1) {
          int j = i + len;
          while (j + 2 <= n) {
            uint8_t l2 = raw[j], t2 = raw[j + 1];
            if (l2 < 2 || j + l2 > n)
              break;
            if (t2 == 0x04) /* next interface — stop */
              break;
            if (t2 == 0x24) { /* CS_INTERFACE */
              uint8_t st = raw[j + 2]; /* subtype */
              uint8_t id = (l2 > 3) ? raw[j + 3] : 0;
              const char *nm = "?";
              switch (st) {
              case 0x01: nm = "header"; break;
              case 0x02: nm = "input_terminal"; break;
              case 0x03: nm = "output_terminal"; break;
              case 0x04: nm = "mixer_unit"; break;
              case 0x05: nm = "selector_unit"; break;
              case 0x06: nm = "feature_unit"; break;
              case 0x07: nm = "processing_unit"; break;
              case 0x08: nm = "extension_unit"; break;
              }
              cJSON *ent = cJSON_CreateObject();
              cJSON_AddStringToObject(ent, "type", nm);
              cJSON_AddNumberToObject(ent, "id", id);
              if (st == 0x06 && l2 >= 8) { /* FU: master controls @ len-4.. */
                uint16_t ctrl = (uint16_t)(raw[j + l2 - 4] |
                                           (raw[j + l2 - 3] << 8));
                cJSON_AddNumberToObject(ent, "controls", ctrl);
              }
              if (st == 0x02 && l2 >= 12) { /* IT: terminal type + src */
                uint16_t tt = (uint16_t)(raw[j + 4] | (raw[j + 5] << 8));
                cJSON_AddNumberToObject(ent, "term_type", tt);
                cJSON_AddNumberToObject(ent, "src", raw[j + 10]);
              }
              if (st == 0x03 && l2 >= 9) { /* OT: terminal type + dst */
                uint16_t tt = (uint16_t)(raw[j + 4] | (raw[j + 5] << 8));
                cJSON_AddNumberToObject(ent, "term_type", tt);
                cJSON_AddNumberToObject(ent, "dst", raw[j + 8]);
              }
              cJSON_AddItemToArray(ifs, ent);
            } else if (t2 == 0x25) { /* CS_ENDPOINT (audio) */
              uint8_t at = raw[j + 2]; /* attributes (sync type) */
              cJSON *ent = cJSON_CreateObject();
              cJSON_AddStringToObject(ent, "type", "cs_endpoint");
              cJSON_AddNumberToObject(ent, "attr", at);
              cJSON_AddItemToArray(ifs, ent);
            }
            j += l2;
          }
        }
      }
      i += len;
    }
  }
  char *s = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!s) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, s);
  free(s);
  return ESP_OK;
}

/* USB 声卡音频编码状态 (audio encoding status of the attached USB sound
 * card) — feeds the web UI's "音频编码状态 / Audio Encoding Status" module. */
static esp_err_t usb_audio_status_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  usb_audio_status_t st;
  bool attached = audio_output_get_usb_audio_status(&st);
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "attached", attached);
  /* The altsetting menu + attach-stage trace are ALWAYS reported (not only
   * when attached), so a device that fails to enumerate can still be
   * diagnosed from the web page — the OTG port occupies the debug UART. */
  {
    usb_alt_info_t alts[12];
    int n = audio_output_get_alt_info(alts, 12);
    cJSON *arr = cJSON_AddArrayToObject(json, "alts");
    for (int i = 0; i < n; i++) {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "rate", alts[i].rate);
      cJSON_AddNumberToObject(o, "bits", alts[i].bits);
      cJSON_AddNumberToObject(o, "subslot", alts[i].subslot);
      cJSON_AddNumberToObject(o, "mps", alts[i].mps);
      cJSON_AddNumberToObject(o, "sync", alts[i].sync);
      cJSON_AddNumberToObject(o, "out", alts[i].out);
      cJSON_AddNumberToObject(o, "ch", alts[i].channels);
      cJSON_AddNumberToObject(o, "iface", alts[i].iface);
      cJSON_AddNumberToObject(o, "alt", alts[i].alt);
      {
        cJSON *ra = cJSON_AddArrayToObject(o, "rates");
        for (int ri = 0; ri < alts[i].rates_n; ri++)
          cJSON_AddItemToArray(ra, cJSON_CreateNumber(alts[i].rates[ri]));
      }
      cJSON_AddItemToArray(arr, o);
    }
  }
  cJSON_AddNumberToObject(json, "setup_stage", st.setup_stage);
  cJSON_AddNumberToObject(json, "setup_err", st.setup_err);
  /* v1.1: the receiver-side pre-roll (180/60/20 ms) currently in effect. */
  {
    uint8_t mode = settings_get_latency_mode();
    static const int k_preroll[3] = {180, 60, 20};
    cJSON_AddNumberToObject(json, "latency_mode", mode);
    cJSON_AddNumberToObject(json, "preroll_ms",
                            k_preroll[mode <= 2 ? mode : 0]);
  }
  if (attached) {
    cJSON_AddNumberToObject(json, "sample_rate", st.sample_rate);
    cJSON_AddNumberToObject(json, "rate_readback", st.rate_readback);
    cJSON_AddNumberToObject(json, "bits", st.bits);
    cJSON_AddNumberToObject(json, "channels", st.channels);
    cJSON_AddNumberToObject(json, "uac_version", st.uac_version);
    cJSON_AddBoolToObject(json, "streaming", st.streaming);
    cJSON_AddNumberToObject(json, "underruns", st.underruns);
    cJSON_AddNumberToObject(json, "fifo_bytes", st.fifo_bytes);
    cJSON_AddNumberToObject(json, "fifo_cap", st.fifo_cap);
    cJSON_AddBoolToObject(json, "hid_active", st.hid_active);
    cJSON_AddNumberToObject(json, "hid_events", st.hid_events);
    cJSON_AddNumberToObject(json, "hid_volup", st.hid_volup);
    cJSON_AddNumberToObject(json, "hid_voldown", st.hid_voldown);
    cJSON_AddNumberToObject(json, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    if (st.hid_last_n) {
      cJSON *ra = cJSON_AddArrayToObject(json, "hid_last_raw");
      for (int i = 0; i < st.hid_last_n; i++) {
        cJSON_AddItemToArray(ra, cJSON_CreateNumber(st.hid_last_raw[i]));
      }
    }
    if (st.hid_press_n) {
      cJSON *rp = cJSON_AddArrayToObject(json, "hid_press_raw");
      for (int i = 0; i < st.hid_press_n; i++) {
        cJSON_AddItemToArray(rp, cJSON_CreateNumber(st.hid_press_raw[i]));
      }
    }
    cJSON_AddStringToObject(json, "product", st.product);
  }
  /* Volume telemetry: the AirPlay/source volume (phone), the independent
   * device volume (web slider = the card's own volume, live-read so a remote
   * change shows up), the card's FU volume range, and the measured output
   * peak. peak_dbfs near 0 at max settings means the digital path is full
   * scale and a quiet speaker is caused by the speaker's own gain stage. */
  {
#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
    /* Refresh the FU readback first so the value below tracks the remote. */
    audio_output_refresh_fu_volume();
#endif
    int32_t q15 = airplay_get_volume_q15();
    float ap_db = (q15 > 0) ? 20.0f * log10f((float)q15 / 32768.0f) : -99.0f;
    cJSON_AddNumberToObject(json, "airplay_volume_db", ap_db);
    cJSON_AddNumberToObject(json, "device_volume_db",
                            audio_output_get_device_volume_db());
#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
    cJSON_AddNumberToObject(json, "fu_min_db",
                            audio_output_get_fu_min_db());
    cJSON_AddNumberToObject(json, "fu_max_db",
                            audio_output_get_fu_max_db());
    cJSON_AddNumberToObject(json, "fu_vol_db",
                            audio_output_get_fu_vol_db());
    cJSON_AddNumberToObject(json, "fu_ch1_db", st.fu_ch1_db);
    cJSON_AddNumberToObject(json, "fu_ch2_db", st.fu_ch2_db);
    cJSON_AddBoolToObject(json, "fu_volume",
                          audio_output_has_fu_volume());
    {
      fu_volume_probe_t pr;
      audio_output_get_fu_probe(&pr);
      cJSON_AddBoolToObject(json, "fu_probe_ran", pr.probe_ran);
      cJSON_AddBoolToObject(json, "fu_probe_range_ok", pr.range_ok);
      cJSON_AddNumberToObject(json, "fu_probe_min_db", pr.min_db);
      cJSON_AddNumberToObject(json, "fu_probe_max_db", pr.max_db);
      cJSON_AddBoolToObject(json, "fu_probe_set0_ok", pr.set0_ok);
      cJSON_AddBoolToObject(json, "fu_probe_get0_ok", pr.get0_ok);
      cJSON_AddNumberToObject(json, "fu_probe_get0_db", pr.get0_db);
      cJSON_AddBoolToObject(json, "fu_probe_set6_ok", pr.set6_ok);
      cJSON_AddBoolToObject(json, "fu_probe_get6_ok", pr.get6_ok);
      cJSON_AddNumberToObject(json, "fu_probe_get6_db", pr.get6_db);
      cJSON_AddNumberToObject(json, "fu_probe_max_db", pr.max_probe_db);
      cJSON_AddNumberToObject(json, "fu_get_fail_count", pr.get_fail_count);
      cJSON_AddBoolToObject(json, "fu_get_ever_ok", pr.get_ever_ok);
    }
    /* Per-FU table: topology + factory default + live volume. This is what
     * reveals which FU actually sits in the audible path (KEF EGG has
     * several; a topology guess can pick the wrong one). */
    {
      fu_info_t fus[8];
      int n = audio_output_get_fu_info(fus, 8);
      cJSON *arr = cJSON_AddArrayToObject(json, "fus");
      for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", fus[i].id);
        cJSON_AddNumberToObject(o, "src", fus[i].src);
        cJSON_AddBoolToObject(o, "spk", fus[i].is_spk);
        cJSON_AddBoolToObject(o, "mic", fus[i].is_mic);
        cJSON_AddNumberToObject(o, "default_db", fus[i].default_db);
        cJSON_AddNumberToObject(o, "cur_db", fus[i].cur_db);
        cJSON_AddBoolToObject(o, "read_ok", fus[i].read_ok);
        cJSON_AddItemToArray(arr, o);
      }
    }
#endif
    cJSON_AddNumberToObject(json, "peak_dbfs", audio_output_get_peak_dbfs());
  }
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}
#endif /* CONFIG_AUDIO_OUTPUT_USB_HOST */

static esp_err_t system_restart_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  ESP_LOGI(TAG, "Restart requested via web interface");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

/* ================================================================== */
/*  SPIFFS File Management API                                         */
/* ================================================================== */

// Allowed path prefixes for file upload (prevent writes outside SPIFFS)
static const char *ALLOWED_PREFIXES[] = {"/spiffs/"};

static bool is_path_allowed(const char *path) {
  for (int i = 0; i < sizeof(ALLOWED_PREFIXES) / sizeof(ALLOWED_PREFIXES[0]);
       i++) {
    if (strncmp(path, ALLOWED_PREFIXES[i], strlen(ALLOWED_PREFIXES[i])) == 0) {
      // Reject path traversal
      if (strstr(path, "..") != NULL) {
        return false;
      }
      return true;
    }
  }
  return false;
}

static esp_err_t fs_upload_handler(httpd_req_t *req) {
  // Get target path from query string
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  /* The body is streamed to the file a chunk at a time and never held whole,
   * so this is only here to keep a runaway request from filling the 1.9MB
   * SPIFFS. It has to clear the largest asset we upload, which is the display
   * background at ~110KB. */
  if (req->content_len == 0 || req->content_len > (size_t)(256 * 1024)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Body required (max 256KB)");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create %s", path);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create file");
    return ESP_FAIL;
  }

  char buf[SPIFFS_CHUNK_SIZE];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }
    fwrite(buf, 1, (size_t)received, f);
    remaining -= (size_t)received;
  }
  fclose(f);

  ESP_LOGI(TAG, "Uploaded %u bytes to %s", (unsigned)req->content_len, path);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "size", (double)req->content_len);
  cJSON_AddStringToObject(json, "path", path);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_delete_handler(httpd_req_t *req) {
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_CreateObject();
  if (remove(path) == 0) {
    ESP_LOGI(TAG, "Deleted %s", path);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "File not found");
  }
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* Pulling a file back off the device is the only way to take an exact copy of
 * a tuned hybrid flow, whose committed coefficients live nowhere else. */
static esp_err_t fs_download_handler(httpd_req_t *req) {
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  char buf[SPIFFS_CHUNK_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t fs_list_handler(httpd_req_t *req) {
  char query[128] = {0};
  char dir_path[64] = "/spiffs";

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dir_path, sizeof(dir_path));
  }

  if (!is_path_allowed(dir_path) && strcmp(dir_path, "/spiffs") != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  DIR *d = opendir(dir_path);
  cJSON *json = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();

  if (d) {
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entry->d_name);

      char full_path[320];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
      struct stat st;
      if (stat(full_path, &st) == 0) {
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
      }
      cJSON_AddItemToArray(files, item);
    }
    closedir(d);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "Cannot open directory");
  }

  cJSON_AddItemToObject(json, "files", files);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* ================================================================== */
/*  HybridFlow 1 tuning  (only when TAS57xx DAC is configured)         */
/* ================================================================== */

#ifdef CONFIG_DAC_TAS57XX

/* A float widened to double carries its representation error into the JSON
 * (0.707f becomes 0.707000017166), which then shows up in the UI's inputs. */
static double hf1_round(float v, double scale) {
  return round((double)v * scale) / scale;
}

static void hf1_add_float(cJSON *o, const char *name, float v) {
  cJSON_AddNumberToObject(o, name, hf1_round(v, 1e4));
}

static cJSON *hf1_bq_to_json(const tas57xx_bq_t *bq) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "type", (double)bq->type);
  cJSON_AddNumberToObject(o, "sub", (double)bq->subtype);
  hf1_add_float(o, "freq", bq->freq_hz);
  hf1_add_float(o, "q", bq->q);
  hf1_add_float(o, "gain", bq->gain_db);
  cJSON *c = cJSON_AddArrayToObject(o, "coeff");
  for (int i = 0; i < TAS57XX_BQ_WORDS; i++) {
    // 8 places keeps the DSP's 1.23 resolution, which 4 would quantise away.
    cJSON_AddItemToArray(c, cJSON_CreateNumber(hf1_round(bq->coeff[i], 1e8)));
  }
  return o;
}

static void hf1_bq_from_json(const cJSON *o, tas57xx_bq_t *bq) {
  if (!cJSON_IsObject(o)) {
    return;
  }
  const cJSON *v = cJSON_GetObjectItem(o, "type");
  if (json_int_in_range(v, 0, TAS57XX_BQ_TYPE_MAX - 1)) {
    bq->type = (tas57xx_bq_type_t)v->valueint;
  }
  v = cJSON_GetObjectItem(o, "sub");
  if (json_int_in_range(v, 0, TAS57XX_BQ_SUB_MAX - 1)) {
    bq->subtype = (tas57xx_bq_subtype_t)v->valueint;
  }
  v = cJSON_GetObjectItem(o, "freq");
  if (cJSON_IsNumber(v)) {
    bq->freq_hz = (float)v->valuedouble;
  }
  v = cJSON_GetObjectItem(o, "q");
  if (cJSON_IsNumber(v)) {
    bq->q = (float)v->valuedouble;
  }
  v = cJSON_GetObjectItem(o, "gain");
  if (cJSON_IsNumber(v)) {
    bq->gain_db = (float)v->valuedouble;
  }
  v = cJSON_GetObjectItem(o, "coeff");
  if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == TAS57XX_BQ_WORDS) {
    for (int i = 0; i < TAS57XX_BQ_WORDS; i++) {
      const cJSON *n = cJSON_GetArrayItem(v, i);
      if (cJSON_IsNumber(n)) {
        bq->coeff[i] = (float)n->valuedouble;
      }
    }
  }
}

static void hf1_num_from_json(const cJSON *parent, const char *key,
                              float *dst) {
  const cJSON *v = cJSON_GetObjectItem(parent, key);
  if (cJSON_IsNumber(v)) {
    *dst = (float)v->valuedouble;
  }
}

static cJSON *hf1_config_to_json(const tas57xx_hf1_config_t *cfg) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "available", dac_tas57xx_hf1_available());
  cJSON_AddNumberToObject(root, "sample_rate", (double)cfg->sample_rate_hz);

  cJSON *eq = cJSON_AddArrayToObject(root, "eq");
  for (int i = 0; i < TAS57XX_HF1_EQ_BANDS; i++) {
    cJSON_AddItemToArray(eq, hf1_bq_to_json(&cfg->eq[i]));
  }

  cJSON *pbe = cJSON_AddObjectToObject(root, "pbe");
  hf1_add_float(pbe, "hpf", cfg->pbe.hpf_hz);
  cJSON_AddNumberToObject(pbe, "harmonic", cfg->pbe.harmonic);
  cJSON_AddNumberToObject(pbe, "effect", cfg->pbe.effect);
  cJSON_AddBoolToObject(pbe, "enabled", cfg->pbe_enabled);

  cJSON *dbe = cJSON_AddObjectToObject(root, "dbe");
  cJSON *hi = cJSON_AddArrayToObject(dbe, "high");
  cJSON *lo = cJSON_AddArrayToObject(dbe, "low");
  for (int i = 0; i < TAS57XX_HF1_DBE_EQ_BANDS; i++) {
    cJSON_AddItemToArray(hi, hf1_bq_to_json(&cfg->dbe_high[i]));
    cJSON_AddItemToArray(lo, hf1_bq_to_json(&cfg->dbe_low[i]));
  }
  hf1_add_float(dbe, "lower_db", cfg->dbe_lower_db);
  hf1_add_float(dbe, "upper_db", cfg->dbe_upper_db);
  hf1_add_float(dbe, "sense_lo", cfg->sense_lower_hz);
  hf1_add_float(dbe, "sense_hi", cfg->sense_upper_hz);
  hf1_add_float(dbe, "window_ms", cfg->sense_window_ms);

  cJSON *drc = cJSON_AddObjectToObject(root, "drc");
  cJSON *cross = cJSON_AddArrayToObject(drc, "cross");
  for (int i = 0; i < TAS57XX_HF1_DRC_CROSS_SECTIONS; i++) {
    cJSON_AddItemToArray(cross, hf1_bq_to_json(&cfg->drc_cross[i]));
  }
  cJSON *mix = cJSON_AddArrayToObject(drc, "mix");
  for (int i = 0; i < TAS57XX_HF1_DRC_BANDS; i++) {
    cJSON_AddItemToArray(mix,
                         cJSON_CreateNumber(hf1_round(cfg->drc_mix[i], 1e4)));
  }
  cJSON *timing = cJSON_AddArrayToObject(drc, "timing");
  for (int i = 0; i < TAS57XX_HF1_DRC_BANDS; i++) {
    cJSON *t = cJSON_CreateObject();
    hf1_add_float(t, "energy", cfg->drc_timing[i].energy_ms);
    hf1_add_float(t, "attack", cfg->drc_timing[i].attack_ms);
    hf1_add_float(t, "decay", cfg->drc_timing[i].decay_ms);
    cJSON_AddItemToArray(timing, t);
  }
  cJSON *regions = cJSON_AddArrayToObject(drc, "regions");
  for (int i = 0; i < TAS57XX_HF1_DRC_REGIONS; i++) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "mode", cfg->drc_region[i].mode);
    hf1_add_float(r, "ratio", cfg->drc_region[i].ratio);
    cJSON_AddItemToArray(regions, r);
  }
  hf1_add_float(drc, "thresh1", cfg->drc_thresh1_db);
  hf1_add_float(drc, "thresh2", cfg->drc_thresh2_db);

  hf1_add_float(root, "smooth_clip", cfg->smooth_clip_db);
  hf1_add_float(root, "fine_volume", cfg->fine_volume_db);
  return root;
}

/* Overlays whatever the body supplies onto the current tuning, so a client can
 * send one section without having to echo the rest back correctly. */
static void hf1_config_from_json(const cJSON *root, tas57xx_hf1_config_t *cfg) {
  const cJSON *v = cJSON_GetObjectItem(root, "sample_rate");
  if (json_int_in_range(v, 8000, 192000)) {
    cfg->sample_rate_hz = (uint32_t)v->valueint;
  }

  const cJSON *eq = cJSON_GetObjectItem(root, "eq");
  if (cJSON_IsArray(eq)) {
    int n = cJSON_GetArraySize(eq);
    for (int i = 0; i < n && i < TAS57XX_HF1_EQ_BANDS; i++) {
      hf1_bq_from_json(cJSON_GetArrayItem(eq, i), &cfg->eq[i]);
    }
  }

  const cJSON *pbe = cJSON_GetObjectItem(root, "pbe");
  if (cJSON_IsObject(pbe)) {
    hf1_num_from_json(pbe, "hpf", &cfg->pbe.hpf_hz);
    v = cJSON_GetObjectItem(pbe, "harmonic");
    if (json_int_in_range(v, 0, TAS57XX_HF1_PBE_HARMONIC_MAX)) {
      cfg->pbe.harmonic = v->valueint;
    }
    v = cJSON_GetObjectItem(pbe, "effect");
    if (json_int_in_range(v, TAS57XX_HF1_PBE_EFFECT_MIN,
                          TAS57XX_HF1_PBE_EFFECT_MAX)) {
      cfg->pbe.effect = v->valueint;
    }
    v = cJSON_GetObjectItem(pbe, "enabled");
    if (cJSON_IsBool(v)) {
      cfg->pbe_enabled = cJSON_IsTrue(v);
    }
  }

  const cJSON *dbe = cJSON_GetObjectItem(root, "dbe");
  if (cJSON_IsObject(dbe)) {
    const cJSON *hi = cJSON_GetObjectItem(dbe, "high");
    const cJSON *lo = cJSON_GetObjectItem(dbe, "low");
    for (int i = 0; i < TAS57XX_HF1_DBE_EQ_BANDS; i++) {
      if (cJSON_IsArray(hi)) {
        hf1_bq_from_json(cJSON_GetArrayItem(hi, i), &cfg->dbe_high[i]);
      }
      if (cJSON_IsArray(lo)) {
        hf1_bq_from_json(cJSON_GetArrayItem(lo, i), &cfg->dbe_low[i]);
      }
    }
    hf1_num_from_json(dbe, "lower_db", &cfg->dbe_lower_db);
    hf1_num_from_json(dbe, "upper_db", &cfg->dbe_upper_db);
    hf1_num_from_json(dbe, "sense_lo", &cfg->sense_lower_hz);
    hf1_num_from_json(dbe, "sense_hi", &cfg->sense_upper_hz);
    hf1_num_from_json(dbe, "window_ms", &cfg->sense_window_ms);
  }

  const cJSON *drc = cJSON_GetObjectItem(root, "drc");
  if (cJSON_IsObject(drc)) {
    const cJSON *cross = cJSON_GetObjectItem(drc, "cross");
    if (cJSON_IsArray(cross) &&
        cJSON_GetArraySize(cross) == TAS57XX_HF1_DRC_CROSS_SECTIONS) {
      for (int i = 0; i < TAS57XX_HF1_DRC_CROSS_SECTIONS; i++) {
        hf1_bq_from_json(cJSON_GetArrayItem(cross, i), &cfg->drc_cross[i]);
      }
    }
    const cJSON *mix = cJSON_GetObjectItem(drc, "mix");
    if (cJSON_IsArray(mix) &&
        cJSON_GetArraySize(mix) == TAS57XX_HF1_DRC_BANDS) {
      for (int i = 0; i < TAS57XX_HF1_DRC_BANDS; i++) {
        const cJSON *v = cJSON_GetArrayItem(mix, i);
        if (cJSON_IsNumber(v)) {
          cfg->drc_mix[i] = (float)v->valuedouble;
        }
      }
    }
    const cJSON *timing = cJSON_GetObjectItem(drc, "timing");
    for (int i = 0; cJSON_IsArray(timing) && i < TAS57XX_HF1_DRC_BANDS; i++) {
      const cJSON *t = cJSON_GetArrayItem(timing, i);
      if (cJSON_IsObject(t)) {
        hf1_num_from_json(t, "energy", &cfg->drc_timing[i].energy_ms);
        hf1_num_from_json(t, "attack", &cfg->drc_timing[i].attack_ms);
        hf1_num_from_json(t, "decay", &cfg->drc_timing[i].decay_ms);
      }
    }
    const cJSON *regions = cJSON_GetObjectItem(drc, "regions");
    for (int i = 0; cJSON_IsArray(regions) && i < TAS57XX_HF1_DRC_REGIONS;
         i++) {
      const cJSON *r = cJSON_GetArrayItem(regions, i);
      if (cJSON_IsObject(r)) {
        v = cJSON_GetObjectItem(r, "mode");
        if (json_int_in_range(v, 0, TAS57XX_HF1_DRC_EXPAND)) {
          cfg->drc_region[i].mode = v->valueint;
        }
        hf1_num_from_json(r, "ratio", &cfg->drc_region[i].ratio);
      }
    }
    hf1_num_from_json(drc, "thresh1", &cfg->drc_thresh1_db);
    hf1_num_from_json(drc, "thresh2", &cfg->drc_thresh2_db);
  }

  hf1_num_from_json(root, "smooth_clip", &cfg->smooth_clip_db);
  hf1_num_from_json(root, "fine_volume", &cfg->fine_volume_db);
}

static esp_err_t hf1_send_result(httpd_req_t *req, esp_err_t err) {
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
  if (err != ESP_OK) {
    cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
  }
  char *s = cJSON_PrintUnformatted(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
  free(s);
  cJSON_Delete(resp);
  return ESP_OK;
}

/* One page for every flow: it asks which one is loaded and shows that, and
 * offers to swap between the bases that are present. A board only ever runs
 * one at a time, so there is no reason to ship the machinery twice. */
static esp_err_t hf_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/hf.html", "text/html");
}

static esp_err_t hf1_get_handler(httpd_req_t *req) {
  tas57xx_hf1_config_t cfg;
  if (dac_tas57xx_hf1_get(&cfg) != ESP_OK) {
    return hf1_send_result(req, ESP_ERR_INVALID_STATE);
  }
  cJSON *root = hf1_config_to_json(&cfg);
  char *s = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
  free(s);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t hf1_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 12288);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required (max 8KB)");
    return ESP_FAIL;
  }
  cJSON *root = cJSON_Parse(content);
  free(content);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  tas57xx_hf1_config_t cfg;
  esp_err_t err = dac_tas57xx_hf1_get(&cfg);
  if (err == ESP_OK) {
    hf1_config_from_json(root, &cfg);
    err = dac_tas57xx_hf1_set(&cfg);
  }
  cJSON_Delete(root);
  return hf1_send_result(req, err);
}

static esp_err_t hf1_commit_handler(httpd_req_t *req) {
  return hf1_send_result(req, dac_tas57xx_hf1_commit());
}

static esp_err_t hf1_revert_handler(httpd_req_t *req) {
  return hf1_send_result(req, dac_tas57xx_hf1_revert());
}

/* HF3 reuses the hf1_* JSON helpers above: a biquad is a biquad, and the two
 * flows differ only in how many of them there are and what they feed. */

static cJSON *hf3_way_to_json(const tas57xx_hf3_config_t *cfg, int w) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddItemToObject(o, "crossover", hf1_bq_to_json(&cfg->crossover[w]));
  cJSON *eq = cJSON_AddArrayToObject(o, "eq");
  for (int i = 0; i < TAS57XX_HF3_EQ_BANDS; i++) {
    cJSON_AddItemToArray(eq, hf1_bq_to_json(&cfg->eq[w][i]));
  }
  return o;
}

static cJSON *hf3_config_to_json(const tas57xx_hf3_config_t *cfg) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "available", dac_tas57xx_hf3_available());
  cJSON_AddNumberToObject(root, "sample_rate", (double)cfg->sample_rate_hz);

  cJSON *ways = cJSON_AddArrayToObject(root, "ways");
  for (int w = 0; w < TAS57XX_HF3_WAYS; w++) {
    cJSON_AddItemToArray(ways, hf3_way_to_json(cfg, w));
  }
  cJSON_AddNumberToObject(root, "high_delay", cfg->high_delay_samples);

  cJSON *pbe = cJSON_AddObjectToObject(root, "pbe");
  hf1_add_float(pbe, "hpf", cfg->pbe.hpf_hz);
  cJSON_AddNumberToObject(pbe, "harmonic", cfg->pbe.harmonic);
  cJSON_AddNumberToObject(pbe, "effect", cfg->pbe.effect);
  cJSON_AddBoolToObject(pbe, "enabled", cfg->pbe_enabled);

  cJSON *dbe = cJSON_AddObjectToObject(root, "dbe");
  cJSON *hi = cJSON_AddArrayToObject(dbe, "high");
  for (int i = 0; i < TAS57XX_HF3_DBE_HL_BANDS; i++) {
    cJSON_AddItemToArray(hi, hf1_bq_to_json(&cfg->dbe_high[i]));
  }
  cJSON *lo = cJSON_AddArrayToObject(dbe, "low");
  for (int i = 0; i < TAS57XX_HF3_DBE_LL_BANDS; i++) {
    cJSON_AddItemToArray(lo, hf1_bq_to_json(&cfg->dbe_low[i]));
  }
  hf1_add_float(dbe, "lower_db", cfg->dbe_lower_db);
  hf1_add_float(dbe, "upper_db", cfg->dbe_upper_db);
  hf1_add_float(dbe, "sense_lo", cfg->sense_lower_hz);
  hf1_add_float(dbe, "sense_hi", cfg->sense_upper_hz);
  hf1_add_float(dbe, "window_ms", cfg->sense_window_ms);

  cJSON *drc = cJSON_AddObjectToObject(root, "drc");
  cJSON_AddItemToObject(drc, "split_low", hf1_bq_to_json(&cfg->drc_split_low));
  cJSON_AddItemToObject(drc, "split_high",
                        hf1_bq_to_json(&cfg->drc_split_high));
  hf1_add_float(drc, "mix_low", cfg->drc_mix_low);
  hf1_add_float(drc, "mix_mid", cfg->drc_mix_mid);
  cJSON *timing = cJSON_AddArrayToObject(drc, "timing");
  for (int i = 0; i < TAS57XX_HF3_DRC_BANDS; i++) {
    cJSON *t = cJSON_CreateObject();
    hf1_add_float(t, "energy", cfg->drc_timing[i].energy_ms);
    hf1_add_float(t, "attack", cfg->drc_timing[i].attack_ms);
    hf1_add_float(t, "decay", cfg->drc_timing[i].decay_ms);
    cJSON_AddItemToArray(timing, t);
  }
  cJSON *regions = cJSON_AddArrayToObject(drc, "regions");
  for (int i = 0; i < TAS57XX_HF3_DRC_REGIONS; i++) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "mode", cfg->drc_region[i].mode);
    hf1_add_float(r, "ratio", cfg->drc_region[i].ratio);
    cJSON_AddItemToArray(regions, r);
  }
  hf1_add_float(drc, "thresh1", cfg->drc_thresh1_db);
  hf1_add_float(drc, "thresh2", cfg->drc_thresh2_db);

  hf1_add_float(root, "smooth_clip", cfg->smooth_clip_db);
  return root;
}

/* Overlays whatever the body supplies onto the current tuning. The input mixer
 * is deliberately absent: it selects which channel this speaker plays, and is
 * owned by the channel-mode setting rather than by a tuning. */
static void hf3_config_from_json(const cJSON *root, tas57xx_hf3_config_t *cfg) {
  const cJSON *v = cJSON_GetObjectItem(root, "sample_rate");
  if (json_int_in_range(v, 8000, 192000)) {
    cfg->sample_rate_hz = (uint32_t)v->valueint;
  }
  v = cJSON_GetObjectItem(root, "high_delay");
  if (json_int_in_range(v, 0, TAS57XX_HF3_DELAY_MAX)) {
    cfg->high_delay_samples = v->valueint;
  }

  const cJSON *ways = cJSON_GetObjectItem(root, "ways");
  for (int w = 0; cJSON_IsArray(ways) && w < TAS57XX_HF3_WAYS; w++) {
    const cJSON *way = cJSON_GetArrayItem(ways, w);
    if (!cJSON_IsObject(way)) {
      continue;
    }
    hf1_bq_from_json(cJSON_GetObjectItem(way, "crossover"), &cfg->crossover[w]);
    const cJSON *eq = cJSON_GetObjectItem(way, "eq");
    for (int i = 0; cJSON_IsArray(eq) && i < TAS57XX_HF3_EQ_BANDS; i++) {
      hf1_bq_from_json(cJSON_GetArrayItem(eq, i), &cfg->eq[w][i]);
    }
  }

  const cJSON *pbe = cJSON_GetObjectItem(root, "pbe");
  if (cJSON_IsObject(pbe)) {
    hf1_num_from_json(pbe, "hpf", &cfg->pbe.hpf_hz);
    v = cJSON_GetObjectItem(pbe, "harmonic");
    if (json_int_in_range(v, 0, TAS57XX_HF3_PBE_HARMONIC_MAX)) {
      cfg->pbe.harmonic = v->valueint;
    }
    v = cJSON_GetObjectItem(pbe, "effect");
    if (json_int_in_range(v, TAS57XX_HF3_PBE_EFFECT_MIN,
                          TAS57XX_HF3_PBE_EFFECT_MAX)) {
      cfg->pbe.effect = v->valueint;
    }
    v = cJSON_GetObjectItem(pbe, "enabled");
    if (cJSON_IsBool(v)) {
      cfg->pbe_enabled = cJSON_IsTrue(v);
    }
  }

  const cJSON *dbe = cJSON_GetObjectItem(root, "dbe");
  if (cJSON_IsObject(dbe)) {
    const cJSON *hi = cJSON_GetObjectItem(dbe, "high");
    for (int i = 0; cJSON_IsArray(hi) && i < TAS57XX_HF3_DBE_HL_BANDS; i++) {
      hf1_bq_from_json(cJSON_GetArrayItem(hi, i), &cfg->dbe_high[i]);
    }
    const cJSON *lo = cJSON_GetObjectItem(dbe, "low");
    for (int i = 0; cJSON_IsArray(lo) && i < TAS57XX_HF3_DBE_LL_BANDS; i++) {
      hf1_bq_from_json(cJSON_GetArrayItem(lo, i), &cfg->dbe_low[i]);
    }
    hf1_num_from_json(dbe, "lower_db", &cfg->dbe_lower_db);
    hf1_num_from_json(dbe, "upper_db", &cfg->dbe_upper_db);
    hf1_num_from_json(dbe, "sense_lo", &cfg->sense_lower_hz);
    hf1_num_from_json(dbe, "sense_hi", &cfg->sense_upper_hz);
    hf1_num_from_json(dbe, "window_ms", &cfg->sense_window_ms);
  }

  const cJSON *drc = cJSON_GetObjectItem(root, "drc");
  if (cJSON_IsObject(drc)) {
    hf1_bq_from_json(cJSON_GetObjectItem(drc, "split_low"),
                     &cfg->drc_split_low);
    hf1_bq_from_json(cJSON_GetObjectItem(drc, "split_high"),
                     &cfg->drc_split_high);
    hf1_num_from_json(drc, "mix_low", &cfg->drc_mix_low);
    hf1_num_from_json(drc, "mix_mid", &cfg->drc_mix_mid);
    const cJSON *timing = cJSON_GetObjectItem(drc, "timing");
    for (int i = 0; cJSON_IsArray(timing) && i < TAS57XX_HF3_DRC_BANDS; i++) {
      const cJSON *t = cJSON_GetArrayItem(timing, i);
      if (cJSON_IsObject(t)) {
        hf1_num_from_json(t, "energy", &cfg->drc_timing[i].energy_ms);
        hf1_num_from_json(t, "attack", &cfg->drc_timing[i].attack_ms);
        hf1_num_from_json(t, "decay", &cfg->drc_timing[i].decay_ms);
      }
    }
    const cJSON *regions = cJSON_GetObjectItem(drc, "regions");
    for (int i = 0; cJSON_IsArray(regions) && i < TAS57XX_HF3_DRC_REGIONS;
         i++) {
      const cJSON *r = cJSON_GetArrayItem(regions, i);
      if (cJSON_IsObject(r)) {
        v = cJSON_GetObjectItem(r, "mode");
        if (json_int_in_range(v, 0, TAS57XX_HF3_DRC_EXPAND)) {
          cfg->drc_region[i].mode = v->valueint;
        }
        hf1_num_from_json(r, "ratio", &cfg->drc_region[i].ratio);
      }
    }
    hf1_num_from_json(drc, "thresh1", &cfg->drc_thresh1_db);
    hf1_num_from_json(drc, "thresh2", &cfg->drc_thresh2_db);
  }

  hf1_num_from_json(root, "smooth_clip", &cfg->smooth_clip_db);
}

static esp_err_t hf3_get_handler(httpd_req_t *req) {
  tas57xx_hf3_config_t cfg;
  if (dac_tas57xx_hf3_get(&cfg) != ESP_OK) {
    return hf1_send_result(req, ESP_ERR_INVALID_STATE);
  }
  cJSON *root = hf3_config_to_json(&cfg);
  char *s = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
  free(s);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t hf3_post_handler(httpd_req_t *req) {
  char *content = recv_body(req, 12288);
  if (!content) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required (max 12KB)");
    return ESP_FAIL;
  }
  cJSON *root = cJSON_Parse(content);
  free(content);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  tas57xx_hf3_config_t cfg;
  esp_err_t err = dac_tas57xx_hf3_get(&cfg);
  if (err == ESP_OK) {
    hf3_config_from_json(root, &cfg);
    err = dac_tas57xx_hf3_set(&cfg);
  }
  cJSON_Delete(root);
  return hf1_send_result(req, err);
}

static esp_err_t hf3_commit_handler(httpd_req_t *req) {
  return hf1_send_result(req, dac_tas57xx_hf3_commit());
}

static esp_err_t hf3_revert_handler(httpd_req_t *req) {
  return hf1_send_result(req, dac_tas57xx_hf3_revert());
}

/* ---- Flow selection --------------------------------------------------- */

static esp_err_t hf_flow_get_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "active", dac_tas57xx_active_flow());
  cJSON_AddNumberToObject(root, "sample_rate", dac_tas57xx_flow_sample_rate());
  cJSON *avail = cJSON_AddArrayToObject(root, "available");
  for (int flow = 1; flow <= 3; flow += 2) {
    if (dac_tas57xx_flow_base_available(flow)) {
      cJSON_AddItemToArray(avail, cJSON_CreateNumber(flow));
    }
  }
  cJSON_AddBoolToObject(root, "success", true);
  char *s = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
  free(s);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t hf_flow_post_handler(httpd_req_t *req) {
  char *body = recv_body(req, 128);
  if (body == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *root = cJSON_Parse(body);
  free(body);
  cJSON *flow = root ? cJSON_GetObjectItem(root, "flow") : NULL;
  int want = cJSON_IsNumber(flow) ? flow->valueint : -1;
  cJSON_Delete(root);
  if (want != 0 && want != 1 && want != 3) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected flow 0, 1 or 3");
    return ESP_FAIL;
  }

  /* Bi-amp hands the two amplifier outputs to a woofer and a tweeter, so the
   * first sound after a switch may be going somewhere it should not. Turn the
   * volume down and leave it down until whoever asked has checked. */
  if (want != dac_tas57xx_active_flow()) {
    settings_set_volume(VOLUME_UI_MIN_DB);
  }
  return hf1_send_result(req, dac_tas57xx_select_flow(want));
}

#endif /* CONFIG_DAC_TAS57XX */

/* ================================================================== */
/*  Biquad chains  (only when TAS58xx DAC is configured)               */
/* ================================================================== */

#ifdef CONFIG_DAC_TAS58XX

/* ---------- Parametric biquad chains ---------- */

/* Enough for 15 filters worth of JSON with room for whitespace. */
#define BQ_POST_MAX 6144

static esp_err_t bq_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/bq.html", "text/html");
}

/* Which amplifier a chain belongs to, so the page can label the columns
 * without duplicating the dual-DAC wiring logic. */
static const char *bq_amp_role(int dev) {
  if (dev == 0) {
    return "stereo";
  }
  return dac_tas58xx_get_active_second_pbtl() ? "mono" : "stereo";
}

static cJSON *bq_to_json(const tas58xx_bq_t *bq) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "type", bq->type);
  cJSON_AddNumberToObject(o, "sub", bq->sub);
  cJSON_AddNumberToObject(o, "freq", bq->freq_hz);
  cJSON_AddNumberToObject(o, "q", bq->q);
  cJSON_AddNumberToObject(o, "bw", bq->bandwidth_hz);
  cJSON_AddNumberToObject(o, "gain", bq->gain_db);
  cJSON_AddNumberToObject(o, "ripple", bq->ripple_db);
  cJSON_AddBoolToObject(o, "invert", bq->invert != 0);
  if (bq->type == TAS58XX_BQ_CUSTOM) {
    cJSON *c = cJSON_AddArrayToObject(o, "coeff");
    for (int i = 0; i < 5; i++) {
      cJSON_AddItemToArray(c, cJSON_CreateNumber(bq->coeff[i]));
    }
  }
  return o;
}

/* Missing members keep their default, so the page can send a sparse filter.
 * The values themselves are checked by the driver, which owns the limits. */
static bool bq_from_json(const cJSON *o, tas58xx_bq_t *out) {
  if (!cJSON_IsObject(o)) {
    return false;
  }
  tas58xx_bq_init_bypass(out);

  const cJSON *v = cJSON_GetObjectItem(o, "type");
  if (!json_int_in_range(v, 0, TAS58XX_BQ_TYPE_COUNT - 1)) {
    return false;
  }
  out->type = (uint8_t)v->valueint;

  v = cJSON_GetObjectItem(o, "sub");
  if (v) {
    if (!json_int_in_range(v, 0, TAS58XX_BQ_SUB_COUNT - 1)) {
      return false;
    }
    out->sub = (uint8_t)v->valueint;
  }

  v = cJSON_GetObjectItem(o, "invert");
  if (v) {
    if (!cJSON_IsBool(v)) {
      return false;
    }
    out->invert = cJSON_IsTrue(v) ? 1 : 0;
  }

  static const struct {
    const char *key;
    size_t offset;
  } floats[] = {
      {"freq", offsetof(tas58xx_bq_t, freq_hz)},
      {"q", offsetof(tas58xx_bq_t, q)},
      {"bw", offsetof(tas58xx_bq_t, bandwidth_hz)},
      {"gain", offsetof(tas58xx_bq_t, gain_db)},
      {"ripple", offsetof(tas58xx_bq_t, ripple_db)},
  };
  for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++) {
    v = cJSON_GetObjectItem(o, floats[i].key);
    if (!v) {
      continue;
    }
    if (!cJSON_IsNumber(v)) {
      return false;
    }
    *(float *)((char *)out + floats[i].offset) = (float)v->valuedouble;
  }

  const cJSON *c = cJSON_GetObjectItem(o, "coeff");
  if (c) {
    if (!cJSON_IsArray(c) || cJSON_GetArraySize(c) != 5) {
      return false;
    }
    for (int i = 0; i < 5; i++) {
      const cJSON *n = cJSON_GetArrayItem(c, i);
      if (!cJSON_IsNumber(n)) {
        return false;
      }
      out->coeff[i] = (float)n->valuedouble;
    }
  }
  return true;
}

static esp_err_t bq_get_handler(httpd_req_t *req) {
  const int devices = dac_tas58xx_get_device_count();

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "devices", devices);
  cJSON_AddNumberToObject(json, "channels", TAS58XX_BQ_CHANNELS);
  cJSON_AddNumberToObject(json, "slots", TAS58XX_BQ_SLOTS);
  cJSON_AddNumberToObject(json, "rate", dac_tas58xx_bq_sample_rate());
  cJSON_AddNumberToObject(json, "gain_min", TAS58XX_GAIN_MIN_DB);
  cJSON_AddNumberToObject(json, "gain_max", TAS58XX_GAIN_MAX_DB);

  cJSON *amps = cJSON_AddArrayToObject(json, "amps");
  for (int d = 0; d < devices; d++) {
    cJSON *amp = cJSON_CreateObject();
    cJSON_AddNumberToObject(amp, "index", d);
    cJSON_AddStringToObject(amp, "role", bq_amp_role(d));
    cJSON_AddBoolToObject(amp, "ganged", dac_tas58xx_bq_get_ganged(d));
    cJSON_AddNumberToObject(amp, "mix", dac_tas58xx_get_mix(d));
    cJSON_AddBoolToObject(amp, "pbtl", dac_tas58xx_is_pbtl(d));

    cJSON *gains = cJSON_AddArrayToObject(amp, "gains");
    cJSON *mutes = cJSON_AddArrayToObject(amp, "mutes");
    for (int c = 0; c < TAS58XX_BQ_CHANNELS; c++) {
      cJSON_AddItemToArray(gains,
                           cJSON_CreateNumber(dac_tas58xx_get_gain_db(d, c)));
      cJSON_AddItemToArray(mutes,
                           cJSON_CreateBool(dac_tas58xx_get_ch_mute(d, c)));
    }

    cJSON *chans = cJSON_AddArrayToObject(amp, "channels");
    for (int c = 0; c < TAS58XX_BQ_CHANNELS; c++) {
      tas58xx_bq_t chain[TAS58XX_BQ_SLOTS];
      cJSON *slots = cJSON_CreateArray();
      if (dac_tas58xx_bq_get(d, c, chain)) {
        for (int i = 0; i < TAS58XX_BQ_SLOTS; i++) {
          cJSON_AddItemToArray(slots, bq_to_json(&chain[i]));
        }
      }
      cJSON_AddItemToArray(chans, slots);
    }
    cJSON_AddItemToArray(amps, amp);
  }

  /* Unformatted: the full set already runs to several kilobytes. */
  char *json_str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!json_str) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  return ESP_OK;
}

static esp_err_t bq_post_handler(httpd_req_t *req) {
  char *body = recv_body(req, BQ_POST_MAX);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(body);
  free(body);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  const cJSON *v = cJSON_GetObjectItem(json, "dev");
  if (!json_int_in_range(v, 0, dac_tas58xx_get_device_count() - 1)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'dev'");
    return ESP_FAIL;
  }
  const int dev = v->valueint;

  /* Ganging is applied first: a ganged write only carries the left chain and
   * the driver mirrors it when programming. */
  const cJSON *g = cJSON_GetObjectItem(json, "ganged");
  if (cJSON_IsBool(g)) {
    dac_tas58xx_bq_set_ganged(dev, cJSON_IsTrue(g));
  }

  /* Level and mute are per output rather than per chain, so they ride along
   * with the chain edits instead of needing an endpoint of their own. */
  const cJSON *gain = cJSON_GetObjectItem(json, "gain");
  const cJSON *mute = cJSON_GetObjectItem(json, "mute");
  if (gain || mute) {
    const cJSON *oc = cJSON_GetObjectItem(json, "out");
    if (!json_int_in_range(oc, 0, TAS58XX_BQ_CHANNELS - 1)) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'out'");
      return ESP_FAIL;
    }
    if (gain && !cJSON_IsNumber(gain)) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'gain'");
      return ESP_FAIL;
    }
    if (mute && !cJSON_IsBool(mute)) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'mute'");
      return ESP_FAIL;
    }
    if (gain) {
      dac_tas58xx_set_gain_db(dev, oc->valueint, (float)gain->valuedouble);
      float saved[SETTINGS_AMP_OUTPUTS];
      for (int i = 0; i < SETTINGS_AMP_OUTPUTS; i++) {
        saved[i] = dac_tas58xx_get_gain_db(i / SETTINGS_AMP_CHANNELS,
                                           i % SETTINGS_AMP_CHANNELS);
      }
      settings_set_amp_gain(saved);
    }
    if (mute) {
      dac_tas58xx_set_ch_mute(dev, oc->valueint, cJSON_IsTrue(mute));
      uint8_t saved[SETTINGS_AMP_OUTPUTS];
      for (int i = 0; i < SETTINGS_AMP_OUTPUTS; i++) {
        saved[i] = dac_tas58xx_get_ch_mute(i / SETTINGS_AMP_CHANNELS,
                                           i % SETTINGS_AMP_CHANNELS);
      }
      settings_set_amp_mute(saved);
    }
  }

  const cJSON *mix = cJSON_GetObjectItem(json, "mix");
  if (mix) {
    if (!json_int_in_range(mix, 0, TAS58XX_MIX_COUNT - 1)) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'mix'");
      return ESP_FAIL;
    }
    dac_tas58xx_set_mix(dev, (tas58xx_mix_t)mix->valueint);
    uint8_t saved[SETTINGS_AMPS];
    for (int i = 0; i < SETTINGS_AMPS; i++) {
      saved[i] = (uint8_t)dac_tas58xx_get_mix(i);
    }
    settings_set_amp_mix(saved);
  }

  const cJSON *filters = cJSON_GetObjectItem(json, "filters");
  if (filters) {
    v = cJSON_GetObjectItem(json, "ch");
    if (!json_int_in_range(v, 0, TAS58XX_BQ_CHANNELS - 1)) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad 'ch'");
      return ESP_FAIL;
    }
    const int ch = v->valueint;

    if (!cJSON_IsArray(filters) ||
        cJSON_GetArraySize(filters) != TAS58XX_BQ_SLOTS) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                          "'filters' must hold 15 entries");
      return ESP_FAIL;
    }

    tas58xx_bq_t chain[TAS58XX_BQ_SLOTS];
    for (int i = 0; i < TAS58XX_BQ_SLOTS; i++) {
      if (!bq_from_json(cJSON_GetArrayItem(filters, i), &chain[i])) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad filter entry");
        return ESP_FAIL;
      }
    }

    if (dac_tas58xx_bq_set(dev, ch, chain) != ESP_OK) {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filter rejected");
      return ESP_FAIL;
    }
  }

  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

/* Chain edits stay in RAM until committed, matching the hybrid-flow pages:
 * a tuning can be auditioned and walked away from by rebooting. */
static esp_err_t bq_commit_handler(httpd_req_t *req) {
  if (dac_tas58xx_bq_commit() != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

static esp_err_t bq_revert_handler(httpd_req_t *req) {
  if (dac_tas58xx_bq_revert() != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Revert failed");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

#endif /* CONFIG_DAC_TAS58XX */

esp_err_t web_server_start(uint16_t port) {
  if (s_server) {
    ESP_LOGW(TAG, "Web server already running");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
#ifdef CONFIG_BT_ENABLED
  config.max_open_sockets = 2;   // BT: tighter socket budget (LWIP 12)
  config.send_wait_timeout = 10; // BT/WiFi coexistence slows TCP drain
#else
  config.max_open_sockets = 3; // Limit to save lwIP socket slots for AirPlay
#endif
  config.lru_purge_enable = true; // Reclaim stale sockets when all are in use
  // Slots are allocated up front and httpd_register_uri_handler failures are
  // unchecked, so an undercount silently drops whatever registers last, which
  // is log_stream's /ws/logs. Keep these in step with the handlers below.
  config.max_uri_handlers = 39; // 32 here + /ws/logs, plus spare
#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
  config.max_uri_handlers += 2; // /api/audio/usb + /api/desc (diagnostics)
#endif
  config.max_uri_handlers += 2; // /api/remote/layout get/post
  config.max_uri_handlers += 6; // /api/audio/format + /api/ui/lang + /api/audio/latency (get/post each)
#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
  config.max_uri_handlers += 1; // /api/usb/reprobe
#endif
#ifdef DAC_HAS_SUB_OFFSET
  config.max_uri_handlers += 2; // sub level get/post
#endif
#ifdef DAC_HAS_CH_TRIM
  config.max_uri_handlers += 2; // per-channel level get/post
#endif
#ifdef CONFIG_DAC_TAS58XX
  // dual DAC wiring plus the biquad page/API
  config.max_uri_handlers += 7;
#endif
#ifdef CONFIG_DAC_TAS57XX
  config.max_uri_handlers += 11; // tuning page + HF1/HF3 get/post/commit/revert
#endif
  config.max_resp_headers = 8;
  config.stack_size = 8192;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
    return err;
  }

  // Register handlers
  httpd_uri_t root_uri = {
      .uri = "/", .method = HTTP_GET, .handler = root_handler};
  httpd_register_uri_handler(s_server, &root_uri);

  httpd_uri_t favicon_uri = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
  httpd_register_uri_handler(s_server, &favicon_uri);

  httpd_uri_t logs_uri = {
      .uri = "/logs", .method = HTTP_GET, .handler = logs_page_handler};
  httpd_register_uri_handler(s_server, &logs_uri);

  httpd_uri_t speedtest_page_uri = {.uri = "/speedtest",
                                    .method = HTTP_GET,
                                    .handler = speedtest_page_handler};
  httpd_register_uri_handler(s_server, &speedtest_page_uri);

  httpd_uri_t speedtest_ping_uri = {.uri = "/api/speedtest/ping",
                                    .method = HTTP_GET,
                                    .handler = speedtest_ping_handler};
  httpd_register_uri_handler(s_server, &speedtest_ping_uri);

  httpd_uri_t speedtest_dl_uri = {.uri = "/api/speedtest/download",
                                  .method = HTTP_GET,
                                  .handler = speedtest_download_handler};
  httpd_register_uri_handler(s_server, &speedtest_dl_uri);

  httpd_uri_t speedtest_ul_uri = {.uri = "/api/speedtest/upload",
                                  .method = HTTP_POST,
                                  .handler = speedtest_upload_handler};
  httpd_register_uri_handler(s_server, &speedtest_ul_uri);

  httpd_uri_t wifi_scan_uri = {.uri = "/api/wifi/scan",
                               .method = HTTP_GET,
                               .handler = wifi_scan_handler};
  httpd_register_uri_handler(s_server, &wifi_scan_uri);

  httpd_uri_t wifi_config_uri = {.uri = "/api/wifi/config",
                                 .method = HTTP_POST,
                                 .handler = wifi_config_handler};
  httpd_register_uri_handler(s_server, &wifi_config_uri);

  httpd_uri_t device_name_uri = {.uri = "/api/device/name",
                                 .method = HTTP_POST,
                                 .handler = device_name_handler};
  httpd_register_uri_handler(s_server, &device_name_uri);

  httpd_uri_t led_brightness_get_uri = {.uri = "/api/led/brightness",
                                        .method = HTTP_GET,
                                        .handler = led_brightness_get_handler};
  httpd_register_uri_handler(s_server, &led_brightness_get_uri);

  httpd_uri_t led_brightness_post_uri = {.uri = "/api/led/brightness",
                                         .method = HTTP_POST,
                                         .handler =
                                             led_brightness_post_handler};
  httpd_register_uri_handler(s_server, &led_brightness_post_uri);

  httpd_uri_t channel_mode_get_uri = {.uri = "/api/audio/channel",
                                      .method = HTTP_GET,
                                      .handler = channel_mode_get_handler};
  httpd_register_uri_handler(s_server, &channel_mode_get_uri);

  httpd_uri_t channel_mode_post_uri = {.uri = "/api/audio/channel",
                                       .method = HTTP_POST,
                                       .handler = channel_mode_post_handler};
  httpd_register_uri_handler(s_server, &channel_mode_post_uri);

  httpd_uri_t volume_get_uri = {.uri = "/api/audio/volume",
                                .method = HTTP_GET,
                                .handler = volume_get_handler};
  httpd_register_uri_handler(s_server, &volume_get_uri);

  httpd_uri_t volume_post_uri = {.uri = "/api/audio/volume",
                                 .method = HTTP_POST,
                                 .handler = volume_post_handler};
  httpd_register_uri_handler(s_server, &volume_post_uri);

#ifdef DAC_HAS_SUB_OFFSET
  httpd_uri_t sub_offset_get_uri = {.uri = "/api/audio/sub",
                                    .method = HTTP_GET,
                                    .handler = sub_offset_get_handler};
  httpd_register_uri_handler(s_server, &sub_offset_get_uri);

  httpd_uri_t sub_offset_post_uri = {.uri = "/api/audio/sub",
                                     .method = HTTP_POST,
                                     .handler = sub_offset_post_handler};
  httpd_register_uri_handler(s_server, &sub_offset_post_uri);
#endif

#ifdef DAC_HAS_CH_TRIM
  httpd_uri_t ch_trim_get_uri = {.uri = "/api/audio/channels",
                                 .method = HTTP_GET,
                                 .handler = ch_trim_get_handler};
  httpd_register_uri_handler(s_server, &ch_trim_get_uri);

  httpd_uri_t ch_trim_post_uri = {.uri = "/api/audio/channels",
                                  .method = HTTP_POST,
                                  .handler = ch_trim_post_handler};
  httpd_register_uri_handler(s_server, &ch_trim_post_uri);
#endif

#ifdef CONFIG_DAC_TAS58XX
  httpd_uri_t dual_mode_get_uri = {.uri = "/api/audio/dual",
                                   .method = HTTP_GET,
                                   .handler = dual_mode_get_handler};
  httpd_register_uri_handler(s_server, &dual_mode_get_uri);

  httpd_uri_t dual_mode_post_uri = {.uri = "/api/audio/dual",
                                    .method = HTTP_POST,
                                    .handler = dual_mode_post_handler};
  httpd_register_uri_handler(s_server, &dual_mode_post_uri);
#endif

  httpd_uri_t airplay_mode_get_uri = {.uri = "/api/airplay/mode",
                                      .method = HTTP_GET,
                                      .handler = airplay_mode_get_handler};
  httpd_register_uri_handler(s_server, &airplay_mode_get_uri);

  httpd_uri_t airplay_mode_post_uri = {.uri = "/api/airplay/mode",
                                       .method = HTTP_POST,
                                       .handler = airplay_mode_post_handler};
  httpd_register_uri_handler(s_server, &airplay_mode_post_uri);
  httpd_uri_t remote_layout_get_uri = {.uri = "/api/remote/layout",
                                       .method = HTTP_GET,
                                       .handler = remote_layout_get_handler};
  httpd_register_uri_handler(s_server, &remote_layout_get_uri);
  httpd_uri_t remote_layout_post_uri = {.uri = "/api/remote/layout",
                                        .method = HTTP_POST,
                                        .handler = remote_layout_post_handler};
  httpd_register_uri_handler(s_server, &remote_layout_post_uri);

  httpd_uri_t audio_format_get_uri = {.uri = "/api/audio/format",
                                      .method = HTTP_GET,
                                      .handler = audio_format_get_handler};
  httpd_register_uri_handler(s_server, &audio_format_get_uri);
  httpd_uri_t audio_format_post_uri = {.uri = "/api/audio/format",
                                       .method = HTTP_POST,
                                       .handler = audio_format_post_handler};
  httpd_register_uri_handler(s_server, &audio_format_post_uri);

  httpd_uri_t ui_lang_get_uri = {.uri = "/api/ui/lang",
                                 .method = HTTP_GET,
                                 .handler = ui_lang_get_handler};
  httpd_register_uri_handler(s_server, &ui_lang_get_uri);
  httpd_uri_t ui_lang_post_uri = {.uri = "/api/ui/lang",
                                  .method = HTTP_POST,
                                  .handler = ui_lang_post_handler};
  httpd_register_uri_handler(s_server, &ui_lang_post_uri);

  httpd_uri_t latency_get_uri = {.uri = "/api/audio/latency",
                                 .method = HTTP_GET,
                                 .handler = latency_get_handler};
  httpd_register_uri_handler(s_server, &latency_get_uri);
  httpd_uri_t latency_post_uri = {.uri = "/api/audio/latency",
                                  .method = HTTP_POST,
                                  .handler = latency_post_handler};
  httpd_register_uri_handler(s_server, &latency_post_uri);

#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
  httpd_uri_t usb_reprobe_uri = {.uri = "/api/usb/reprobe",
                                 .method = HTTP_POST,
                                 .handler = usb_reprobe_handler};
  httpd_register_uri_handler(s_server, &usb_reprobe_uri);
#endif

  httpd_uri_t ota_uri = {.uri = "/api/ota/update",
                         .method = HTTP_POST,
                         .handler = ota_update_handler};
  httpd_register_uri_handler(s_server, &ota_uri);

  httpd_uri_t metadata_uri = {.uri = "/api/metadata",
                              .method = HTTP_POST,
                              .handler = metadata_post_handler};
  httpd_register_uri_handler(s_server, &metadata_uri);

  httpd_uri_t system_info_uri = {.uri = "/api/system/info",
                                 .method = HTTP_GET,
                                 .handler = system_info_handler};
  httpd_register_uri_handler(s_server, &system_info_uri);

#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
  httpd_uri_t usb_audio_status_uri = {.uri = "/api/audio/usb",
                                      .method = HTTP_GET,
                                      .handler = usb_audio_status_handler};
  httpd_register_uri_handler(s_server, &usb_audio_status_uri);
  httpd_uri_t usb_desc_uri = {.uri = "/api/desc",
                              .method = HTTP_GET,
                              .handler = usb_desc_handler};
  httpd_register_uri_handler(s_server, &usb_desc_uri);
#endif

  httpd_uri_t system_restart_uri = {.uri = "/api/system/restart",
                                    .method = HTTP_POST,
                                    .handler = system_restart_handler};
  httpd_register_uri_handler(s_server, &system_restart_uri);

  // File management API
  httpd_uri_t fs_upload_uri = {.uri = "/api/fs/upload",
                               .method = HTTP_POST,
                               .handler = fs_upload_handler};
  httpd_register_uri_handler(s_server, &fs_upload_uri);

  httpd_uri_t fs_delete_uri = {.uri = "/api/fs/delete",
                               .method = HTTP_POST,
                               .handler = fs_delete_handler};
  httpd_register_uri_handler(s_server, &fs_delete_uri);

  httpd_uri_t fs_list_uri = {
      .uri = "/api/fs/list", .method = HTTP_GET, .handler = fs_list_handler};
  httpd_register_uri_handler(s_server, &fs_list_uri);

  httpd_uri_t fs_download_uri = {.uri = "/api/fs/download",
                                 .method = HTTP_GET,
                                 .handler = fs_download_handler};
  httpd_register_uri_handler(s_server, &fs_download_uri);

  // Captive portal detection endpoints
  // Apple iOS/macOS
  httpd_uri_t apple_captive1 = {.uri = "/hotspot-detect.html",
                                .method = HTTP_GET,
                                .handler = captive_apple_handler};
  httpd_register_uri_handler(s_server, &apple_captive1);

  httpd_uri_t apple_captive2 = {.uri = "/library/test/success.html",
                                .method = HTTP_GET,
                                .handler = captive_apple_handler};
  httpd_register_uri_handler(s_server, &apple_captive2);

  // Android
  httpd_uri_t android_captive = {.uri = "/generate_204",
                                 .method = HTTP_GET,
                                 .handler = captive_android_handler};
  httpd_register_uri_handler(s_server, &android_captive);

  // Windows
  httpd_uri_t windows_captive = {.uri = "/connecttest.txt",
                                 .method = HTTP_GET,
                                 .handler = captive_windows_handler};
  httpd_register_uri_handler(s_server, &windows_captive);
  windows_captive.uri = "/redirect";
  httpd_register_uri_handler(s_server, &windows_captive);

#ifdef CONFIG_DAC_TAS58XX
  httpd_uri_t bq_page_uri = {
      .uri = "/bq", .method = HTTP_GET, .handler = bq_page_handler};
  httpd_register_uri_handler(s_server, &bq_page_uri);

  httpd_uri_t bq_get_uri = {
      .uri = "/api/bq", .method = HTTP_GET, .handler = bq_get_handler};
  httpd_register_uri_handler(s_server, &bq_get_uri);

  httpd_uri_t bq_post_uri = {
      .uri = "/api/bq", .method = HTTP_POST, .handler = bq_post_handler};
  httpd_register_uri_handler(s_server, &bq_post_uri);

  httpd_uri_t bq_commit_uri = {.uri = "/api/bq/commit",
                               .method = HTTP_POST,
                               .handler = bq_commit_handler};
  httpd_register_uri_handler(s_server, &bq_commit_uri);

  httpd_uri_t bq_revert_uri = {.uri = "/api/bq/revert",
                               .method = HTTP_POST,
                               .handler = bq_revert_handler};
  httpd_register_uri_handler(s_server, &bq_revert_uri);
#endif

#ifdef CONFIG_DAC_TAS57XX
  httpd_uri_t hf_page_uri = {
      .uri = "/hf", .method = HTTP_GET, .handler = hf_page_handler};
  httpd_register_uri_handler(s_server, &hf_page_uri);

  httpd_uri_t hf1_get_uri = {
      .uri = "/api/hf1", .method = HTTP_GET, .handler = hf1_get_handler};
  httpd_register_uri_handler(s_server, &hf1_get_uri);

  httpd_uri_t hf1_post_uri = {
      .uri = "/api/hf1", .method = HTTP_POST, .handler = hf1_post_handler};
  httpd_register_uri_handler(s_server, &hf1_post_uri);

  httpd_uri_t hf1_commit_uri = {.uri = "/api/hf1/commit",
                                .method = HTTP_POST,
                                .handler = hf1_commit_handler};
  httpd_register_uri_handler(s_server, &hf1_commit_uri);

  httpd_uri_t hf1_revert_uri = {.uri = "/api/hf1/revert",
                                .method = HTTP_POST,
                                .handler = hf1_revert_handler};
  httpd_register_uri_handler(s_server, &hf1_revert_uri);

  httpd_uri_t hf3_get_uri = {
      .uri = "/api/hf3", .method = HTTP_GET, .handler = hf3_get_handler};
  httpd_register_uri_handler(s_server, &hf3_get_uri);

  httpd_uri_t hf3_post_uri = {
      .uri = "/api/hf3", .method = HTTP_POST, .handler = hf3_post_handler};
  httpd_register_uri_handler(s_server, &hf3_post_uri);

  httpd_uri_t hf3_commit_uri = {.uri = "/api/hf3/commit",
                                .method = HTTP_POST,
                                .handler = hf3_commit_handler};
  httpd_register_uri_handler(s_server, &hf3_commit_uri);

  httpd_uri_t hf3_revert_uri = {.uri = "/api/hf3/revert",
                                .method = HTTP_POST,
                                .handler = hf3_revert_handler};
  httpd_register_uri_handler(s_server, &hf3_revert_uri);

  httpd_uri_t hf_flow_get_uri = {.uri = "/api/hf/flow",
                                 .method = HTTP_GET,
                                 .handler = hf_flow_get_handler};
  httpd_register_uri_handler(s_server, &hf_flow_get_uri);

  httpd_uri_t hf_flow_post_uri = {.uri = "/api/hf/flow",
                                  .method = HTTP_POST,
                                  .handler = hf_flow_post_handler};
  httpd_register_uri_handler(s_server, &hf_flow_post_uri);
#endif

  log_stream_register(s_server);

  ESP_LOGI(TAG, "Web server started on port %d with captive portal support",
           port);
  return ESP_OK;
}

void web_server_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
  }
}
