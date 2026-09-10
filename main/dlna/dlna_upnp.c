#include "dlna/dlna_upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "dlna/dlna_renderer.h"
#include "audio/audio_output.h"
#include "network/wifi.h"
#include "settings.h"

static const char *TAG = "dlna_upnp";

/* ── UPnP constants ─────────────────────────────────────────────────────── */
#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define HTTP_PORT 80

#define URN_AVT "urn:schemas-upnp-org:service:AVTransport:1"
#define URN_RC "urn:schemas-upnp-org:service:RenderingControl:1"
#define URN_CM "urn:schemas-upnp-org:service:ConnectionManager:1"
#define URN_DEVICE "urn:schemas-upnp-org:device:MediaRenderer:1"
#define URN_ROOT "upnp:rootdevice"

#define ALIVE_INTERVAL_US (30 * 1000 * 1000) /* periodic alive announce */

/* ── State ──────────────────────────────────────────────────────────────── */
static char s_uuid[48] = {0};     /* uuid:xxxxxxxx-... */
static char s_location[128] = {0}; /* http://<ip>:80/description.xml */
static int s_ssdp_fd = -1;
static bool s_running = false;

/* TEMP DIAG: SOAP request counters */
static volatile uint32_t s_soap_posts = 0;
static volatile uint32_t s_soap_play = 0;
static volatile uint32_t s_soap_seturi = 0;
static volatile uint32_t s_soap_avt_calls = 0;

uint32_t dlna_upnp_get_soap_posts(void) { return s_soap_posts; }
uint32_t dlna_upnp_get_soap_play(void) { return s_soap_play; }
uint32_t dlna_upnp_get_soap_seturi(void) { return s_soap_seturi; }
uint32_t dlna_upnp_get_soap_avt_calls(void) { return s_soap_avt_calls; }

/* ── Small helpers ─────────────────────────────────────────────────────── */

static void make_uuid(void) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_uuid, sizeof(s_uuid),
           "uuid:%02x%02x%02x%02x-%02x%02x-4%02x-8%02x-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           0x0 | (mac[0] & 0x0f), 0x8 | (mac[1] & 0x07), mac[2], mac[3],
           mac[4], mac[5], mac[0], mac[1]);
}

static int get_sta_ip(char *ip, size_t sz) {
  if (wifi_get_ip_str(ip, sz) == ESP_OK && ip[0] && strcmp(ip, "0.0.0.0") != 0) {
    return 0;
  }
  /* Fall back to the AP interface (hotspot-only mode). */
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP");
  if (ap) {
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap, &info) == ESP_OK &&
        !ip4_addr_isany_val(info.ip)) {
      snprintf(ip, sz, IPSTR, IP2STR(&info.ip));
      return 0;
    }
  }
  return -1;
}

static void refresh_location(void) {
  char ip[32] = {0};
  if (get_sta_ip(ip, sizeof(ip)) == 0) {
    snprintf(s_location, sizeof(s_location), "http://%s:%d/description.xml",
             ip, HTTP_PORT);
    ESP_LOGI(TAG, "LOCATION=%s", s_location);
  } else {
    ESP_LOGW(TAG, "get_sta_ip failed (ip='%s')", ip);
  }
}

/* Replace XML entities in place (&amp; &lt; &gt; &quot; &apos;). */
static void xml_entity_decode(char *s) {
  char *r = s;
  char *w = s;
  while (*r) {
    if (strncmp(r, "&amp;", 5) == 0) {
      *w++ = '&';
      r += 5;
    } else if (strncmp(r, "&lt;", 4) == 0) {
      *w++ = '<';
      r += 4;
    } else if (strncmp(r, "&gt;", 4) == 0) {
      *w++ = '>';
      r += 4;
    } else if (strncmp(r, "&quot;", 6) == 0) {
      *w++ = '"';
      r += 6;
    } else if (strncmp(r, "&apos;", 6) == 0) {
      *w++ = '\'';
      r += 6;
    } else {
      *w++ = *r++;
    }
  }
  *w = 0;
}

/* Extract <tag>value</tag> from XML. Returns 0 and the value, or -1. */
static int xml_get(const char *body, const char *tag, char *out,
                   size_t out_sz) {
  char open[64];
  char close[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *s = strstr(body, open);
  if (!s) {
    return -1;
  }
  s += strlen(open);
  const char *e = strstr(s, close);
  if (!e) {
    return -1;
  }
  size_t n = (size_t)(e - s);
  if (n >= out_sz) {
    n = out_sz - 1;
  }
  memcpy(out, s, n);
  out[n] = 0;
  return 0;
}

static void fmt_hms(double secs, char *out, size_t sz) {
  int s = (int)secs;
  int h = s / 3600;
  int m = (s % 3600) / 60;
  int r = s % 60;
  snprintf(out, sz, "%02d:%02d:%02d", h, m, r);
}

/* Parse "H:MM:SS" (or "MM:SS") into seconds; returns 0 on success. */
static int parse_hms(const char *in, double *out) {
  int h = 0, m = 0, s = 0;
  if (sscanf(in, "%d:%d:%d", &h, &m, &s) == 3) {
    *out = (double)h * 3600 + (double)m * 60 + (double)s;
    return 0;
  }
  if (sscanf(in, "%d:%d", &m, &s) == 2) {
    *out = (double)m * 60 + (double)s;
    return 0;
  }
  return -1;
}

/* ── SOAP response builders ─────────────────────────────────────────────── */

static void soap_envelope(const char *urn, const char *action,
                          const char *inner, char *out, size_t sz) {
  snprintf(out, sz,
           "<?xml version=\"1.0\"?>\n"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
           "<s:Body><u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse></s:Body>"
           "</s:Envelope>",
           action, urn, inner ? inner : "", action);
}

static void soap_fault(int code, const char *desc, char *out, size_t sz) {
  snprintf(out, sz,
           "<?xml version=\"1.0\"?>\n"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
           "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
           "<faultstring>UPnPError</faultstring><detail>"
           "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
           "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
           "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
           code, desc);
}

/* ── SOAP actions ───────────────────────────────────────────────────────── */

static void avt_action(const char *action, const char *body, char *resp,
                       size_t resp_sz) {
  char inner[1024];
  s_soap_avt_calls++;
  if (strcmp(action, "SetAVTransportURI") == 0) {
    s_soap_seturi++;
    char uri[DLNA_URI_MAX] = {0};
    char meta[DLNA_META_MAX] = {0};
    if (xml_get(body, "CurrentURI", uri, sizeof(uri)) == 0) {
      xml_entity_decode(uri);
    }
    xml_get(body, "CurrentURIMetaData", meta, sizeof(meta));
    xml_entity_decode(meta);
    dlna_renderer_set_uri(uri, meta);
    soap_envelope(URN_AVT, action, "", resp, resp_sz);
    return;
  }
  if (strcmp(action, "Play") == 0) {
    s_soap_play++;
    dlna_renderer_play();
    soap_envelope(URN_AVT, action, "", resp, resp_sz);
    return;
  }
  if (strcmp(action, "Pause") == 0) {
    dlna_renderer_pause();
    soap_envelope(URN_AVT, action, "", resp, resp_sz);
    return;
  }
  if (strcmp(action, "Stop") == 0) {
    dlna_renderer_stop();
    soap_envelope(URN_AVT, action, "", resp, resp_sz);
    return;
  }
  if (strcmp(action, "Seek") == 0) {
    char target[64] = {0};
    double secs = 0.0;
    if (xml_get(body, "Target", target, sizeof(target)) == 0 &&
        parse_hms(target, &secs) == 0) {
      dlna_renderer_seek(secs);
      soap_envelope(URN_AVT, action, "", resp, resp_sz);
    } else {
      soap_fault(402, "Invalid Args", resp, resp_sz);
    }
    return;
  }
  if (strcmp(action, "GetTransportInfo") == 0) {
    const char *state = "STOPPED";
    switch (dlna_renderer_get_state()) {
    case DLNA_STATE_PLAYING:
      state = "PLAYING";
      break;
    case DLNA_STATE_PAUSED:
      state = "PAUSED_PLAYBACK";
      break;
    default:
      break;
    }
    snprintf(inner, sizeof(inner),
             "<CurrentTransportState>%s</CurrentTransportState>"
             "<CurrentTransportStatus>OK</CurrentTransportStatus>"
             "<CurrentSpeed>1</CurrentSpeed>",
             state);
    soap_envelope(URN_AVT, action, inner, resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetPositionInfo") == 0) {
    char dur[16], rel[16];
    fmt_hms(dlna_renderer_get_duration(), dur, sizeof(dur));
    fmt_hms(dlna_renderer_get_position(), rel, sizeof(rel));
    snprintf(inner, sizeof(inner),
             "<Track>1</Track>"
             "<TrackDuration>%s</TrackDuration>"
             "<TrackMetaData>%s</TrackMetaData>"
             "<TrackURI>%s</TrackURI>"
             "<RelTime>%s</RelTime>"
             "<AbsTime>NOT_IMPLEMENTED</AbsTime>"
             "<RelCount>2147483647</RelCount>"
             "<AbsCount>2147483647</AbsCount>",
             dur, dlna_renderer_get_metadata(), dlna_renderer_get_uri(), rel);
    soap_envelope(URN_AVT, action, inner, resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetTransportSettings") == 0) {
    soap_envelope(URN_AVT, action,
                  "<PlayMode>NORMAL</PlayMode>"
                  "<RecQualityMode>EP</RecQualityMode>",
                  resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetMediaInfo") == 0) {
    char dur[16];
    fmt_hms(dlna_renderer_get_duration(), dur, sizeof(dur));
    snprintf(inner, sizeof(inner),
             "<NrTracks>%d</NrTracks>"
             "<MediaDuration>%s</MediaDuration>"
             "<CurrentURI>%s</CurrentURI>"
             "<CurrentURIMetaData>%s</CurrentURIMetaData>"
             "<NextURI></NextURI>"
             "<NextURIMetaData></NextURIMetaData>"
             "<PlayMedium>NONE</PlayMedium>"
             "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
             "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
             dlna_renderer_get_uri()[0] ? 1 : 0, dur, dlna_renderer_get_uri(),
             dlna_renderer_get_metadata());
    soap_envelope(URN_AVT, action, inner, resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetDeviceCapabilities") == 0) {
    soap_envelope(URN_AVT, action,
                  "<PlayMedia>*:*</PlayMedia>"
                  "<RecMedia>NONE</RecMedia>"
                  "<RecQualityModes>EP</RecQualityModes>",
                  resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetCurrentTransportActions") == 0) {
    const char *actions = "Play,Pause,Stop,Seek";
    if (dlna_renderer_get_state() == DLNA_STATE_STOPPED) {
      actions = "Play";
    }
    snprintf(inner, sizeof(inner), "<Actions>%s</Actions>", actions);
    soap_envelope(URN_AVT, action, inner, resp, resp_sz);
    return;
  }

  ESP_LOGW(TAG, "Unhandled AVT action: %s", action);
  soap_fault(401, "Invalid Action", resp, resp_sz);
}

static void rc_action(const char *action, const char *body, char *resp,
                      size_t resp_sz) {
  char inner[512];

  if (strcmp(action, "GetVolume") == 0) {
    snprintf(inner, sizeof(inner), "<CurrentVolume>%d</CurrentVolume>",
             dlna_renderer_get_volume());
    soap_envelope(URN_RC, action, inner, resp, resp_sz);
    return;
  }
  if (strcmp(action, "SetVolume") == 0) {
    char v[32] = {0};
    if (xml_get(body, "DesiredVolume", v, sizeof(v)) == 0) {
      dlna_renderer_set_volume(atoi(v));
      soap_envelope(URN_RC, action, "", resp, resp_sz);
    } else {
      soap_fault(402, "Invalid Args", resp, resp_sz);
    }
    return;
  }
  if (strcmp(action, "GetVolumeDB") == 0) {
    float db = audio_output_get_device_volume_db();
    snprintf(inner, sizeof(inner), "<CurrentVolumeDB>%d</CurrentVolumeDB>",
             (int)(db * 100));
    soap_envelope(URN_RC, action, inner, resp, resp_sz);
    return;
  }
  if (strcmp(action, "SetVolumeDB") == 0) {
    char v[32] = {0};
    if (xml_get(body, "DesiredVolumeDB", v, sizeof(v)) == 0) {
      float db = (float)atoi(v) / 100.0f;
      int pct = (int)((db + 30.0f) / 30.0f * 100.0f + 0.5f);
      dlna_renderer_set_volume(pct);
      soap_envelope(URN_RC, action, "", resp, resp_sz);
    } else {
      soap_fault(402, "Invalid Args", resp, resp_sz);
    }
    return;
  }
  if (strcmp(action, "GetVolumeDBRange") == 0) {
    soap_envelope(URN_RC, action,
                  "<MinValue>-3000</MinValue><MaxValue>0</MaxValue>",
                  resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetMute") == 0) {
    soap_envelope(URN_RC, action, "<CurrentMute>0</CurrentMute>", resp,
                  resp_sz);
    return;
  }
  if (strcmp(action, "SetMute") == 0) {
    soap_envelope(URN_RC, action, "", resp, resp_sz);
    return;
  }

  ESP_LOGW(TAG, "Unhandled RC action: %s", action);
  soap_fault(401, "Invalid Action", resp, resp_sz);
}

/* Supported sink formats advertised via GetProtocolInfo. Order matters for
 * some control points (LPCM first = default). WAV is served as LPCM;
 * OGG/AIFF are advertised as raw container types. */
#define SINK_PROTOCOL_INFO \
  "http-get:*:audio/L16;rate=44100;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=01700000000000000000000000000000," \
  "http-get:*:audio/L16;rate=48000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=01700000000000000000000000000000," \
  "http-get:*:audio/mpeg:DLNA.ORG_PN=MP3," \
  "http-get:*:audio/flac:DLNA.ORG_PN=FLAC," \
  "http-get:*:audio/aac:DLNA.ORG_PN=AAC_ADTS," \
  "http-get:*:audio/ogg:DLNA.ORG_PN=OGG"

static void cm_action(const char *action, const char *body, char *resp,
                      size_t resp_sz) {
  char inner[1024];
  if (strcmp(action, "GetProtocolInfo") == 0) {
    snprintf(inner, sizeof(inner),
             "<Source></Source>"
             "<Sink>%s</Sink>", SINK_PROTOCOL_INFO);
    soap_envelope(URN_CM, action, inner, resp, resp_sz);
    return;
  }
  if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
    soap_envelope(URN_CM, action, "<ConnectionIDs>0</ConnectionIDs>", resp,
                  resp_sz);
    return;
  }
  if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
    soap_envelope(URN_CM, action,
                  "<RcsID>-1</RcsID><AVTransportID>-1</AVTransportID>"
                  "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
                  "<PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction>"
                  "<Status>OK</Status>",
                  resp, resp_sz);
    return;
  }
  ESP_LOGW(TAG, "Unhandled CM action: %s", action);
  soap_fault(401, "Invalid Action", resp, resp_sz);
}

/* ── HTTP handlers ──────────────────────────────────────────────────────── */

static int read_body(httpd_req_t *req, char *buf, size_t sz) {
  int total = 0;
  while (total < req->content_len && total < (int)sz - 1) {
    int remaining = req->content_len - total;
    int cap = (int)sz - 1 - total;
    int r = httpd_req_recv(req, buf + total, remaining < cap ? remaining : cap);
    if (r <= 0) {
      break;
    }
    total += r;
  }
  buf[total] = 0;
  return total;
}

static void send_xml(httpd_req_t *req, int status, const char *body) {
  httpd_resp_set_status(req, status == 200 ? "200 OK" : "500 Internal Server Error");
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_set_hdr(req, "EXT", "");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t description_handler(httpd_req_t *req) {
  char name[65] = {0};
  settings_get_device_name(name, sizeof(name));
  char body[2048];
  snprintf(body, sizeof(body),
           "<?xml version=\"1.0\"?>\n"
           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
           "<specVersion><major>1</major><minor>0</minor></specVersion>"
           "<device>"
           "<deviceType>%s</deviceType>"
           "<friendlyName>%s</friendlyName>"
           "<manufacturer>ESP32-S3 AirPlay2 DLNA</manufacturer>"
           "<manufacturerURL>https://github.com/imalexgao</manufacturerURL>"
           "<modelDescription>ESP32-S3 AirPlay 2 + DLNA Receiver</modelDescription>"
           "<modelName>ESP32-S3-AirPlay2-DLNA</modelName>"
           "<modelNumber>2.0</modelNumber>"
           "<UDN>%s</UDN>"
           "<serviceList>"
           "<service>"
           "<serviceType>%s</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
           "<SCPDURL>/upnp/scpd/avt.xml</SCPDURL>"
           "<controlURL>/upnp/control/avt</controlURL>"
           "<eventSubURL>/upnp/event/avt</eventSubURL>"
           "</service>"
           "<service>"
           "<serviceType>%s</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
           "<SCPDURL>/upnp/scpd/rc.xml</SCPDURL>"
           "<controlURL>/upnp/control/rc</controlURL>"
           "<eventSubURL>/upnp/event/rc</eventSubURL>"
           "</service>"
           "<service>"
           "<serviceType>%s</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
           "<SCPDURL>/upnp/scpd/cm.xml</SCPDURL>"
           "<controlURL>/upnp/control/cm</controlURL>"
           "<eventSubURL>/upnp/event/cm</eventSubURL>"
           "</service>"
           "</serviceList>"
           "</device></root>",
           URN_DEVICE, name, s_uuid, URN_AVT, URN_RC, URN_CM);
  send_xml(req, 200, body);
  return ESP_OK;
}

static esp_err_t soap_handler(httpd_req_t *req, bool avt, bool rc) {
  char body[2048];
  s_soap_posts++;
  ESP_LOGI(TAG, "soap POST start: len=%d avt=%d", req->content_len, avt);
  int got = read_body(req, body, sizeof(body));
  ESP_LOGI(TAG, "soap POST body read: %d bytes", got);

  char action_hdr[128] = {0};
  httpd_req_get_hdr_value_str(req, "SOAPAction", action_hdr,
                              sizeof(action_hdr));
  /* Header: "urn:...:1#ActionName" — extract after '#'. */
  char action[64] = {0};
  const char *hash = strrchr(action_hdr, '#');
  if (hash && *hash) {
    char *end = strchr(hash + 1, '"');
    size_t n = end ? (size_t)(end - hash - 1) : strlen(hash + 1);
    if (n >= sizeof(action)) {
      n = sizeof(action) - 1;
    }
    memcpy(action, hash + 1, n);
    action[n] = 0;
  }

  char resp[2048];
  if (avt) {
    avt_action(action, body, resp, sizeof(resp));
  } else if (rc) {
    rc_action(action, body, resp, sizeof(resp));
  } else {
    cm_action(action, body, resp, sizeof(resp));
  }

  bool fault = strstr(resp, "s:Fault") != NULL;
  send_xml(req, fault ? 500 : 200, resp);
  return ESP_OK;
}

static esp_err_t avt_control_handler(httpd_req_t *req) {
  return soap_handler(req, true, false);
}

static esp_err_t rc_control_handler(httpd_req_t *req) {
  return soap_handler(req, false, true);
}

static esp_err_t cm_control_handler(httpd_req_t *req) {
  return soap_handler(req, false, false);
}

static esp_err_t event_sub_handler(httpd_req_t *req) {
  /* Acknowledge GENA subscribe/unsubscribe; no event push in M1 (control
   * points poll GetPositionInfo anyway). */
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_hdr(req, "SID",
                     "uuid:dlna-event-00000000000000000000000000000000");
  httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
  httpd_resp_set_hdr(req, "Content-Length", "0");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t cm_scpd_handler(httpd_req_t *req) {
  static const char scpd[] =
      "<?xml version=\"1.0\"?>\n"
      "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion>"
      "<actionList>"
      "<action><name>GetProtocolInfo</name><argumentList>"
      "<argument><name>Source</name><direction>out</direction>"
      "<relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
      "<argument><name>Sink</name><direction>out</direction>"
      "<relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetCurrentConnectionIDs</name><argumentList>"
      "<argument><name>ConnectionIDs</name><direction>out</direction>"
      "<relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetCurrentConnectionInfo</name><argumentList>"
      "<argument><name>ConnectionID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
      "<argument><name>RcsID</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>"
      "<argument><name>AVTransportID</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>"
      "<argument><name>ProtocolInfo</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>"
      "<argument><name>PeerConnectionManager</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>"
      "<argument><name>PeerConnectionID</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
      "<argument><name>Direction</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>"
      "<argument><name>Status</name><direction>out</direction>"
      "<relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>"
      "</argumentList></action>"
      "</actionList>"
      "<serviceStateTable>"
      "<stateVariable sendEvents=\"no\"><name>SourceProtocolInfo</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>SinkProtocolInfo</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentConnectionIDs</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionID</name>"
      "<dataType>ui4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_RcsID</name>"
      "<dataType>i4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_AVTransportID</name>"
      "<dataType>i4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ProtocolInfo</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionManager</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Direction</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionStatus</name>"
      "<dataType>string</dataType></stateVariable>"
      "</serviceStateTable>"
      "</scpd>";
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_set_hdr(req, "EXT", "");
  httpd_resp_send(req, scpd, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t avt_scpd_handler(httpd_req_t *req) {
  static const char scpd[] =
      "<?xml version=\"1.0\"?>\n"
      "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion>"
      "<actionList>"
      "<action><name>SetAVTransportURI</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
      "<argument><name>CurrentURI</name><direction>in</direction>"
      "<relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
      "<argument><name>CurrentURIMetaData</name><direction>in</direction>"
      "<relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>Play</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Speed</name><direction>in</direction>"
      "<relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>Pause</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>Stop</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>Seek</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Unit</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>"
      "<argument><name>Target</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetTransportInfo</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>CurrentTransportState</name><direction>out</direction>"
      "<relatedStateVariable>TransportState</relatedStateVariable></argument>"
      "<argument><name>CurrentTransportStatus</name><direction>out</direction>"
      "<relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
      "<argument><name>CurrentSpeed</name><direction>out</direction>"
      "<relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetPositionInfo</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Track</name><direction>out</direction>"
      "<relatedStateVariable>CurrentTrack</relatedStateVariable></argument>"
      "<argument><name>TrackDuration</name><direction>out</direction>"
      "<relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>"
      "<argument><name>TrackMetaData</name><direction>out</direction>"
      "<relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>"
      "<argument><name>TrackURI</name><direction>out</direction>"
      "<relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>"
      "<argument><name>RelTime</name><direction>out</direction>"
      "<relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>"
      "<argument><name>AbsTime</name><direction>out</direction>"
      "<relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>"
      "<argument><name>RelCount</name><direction>out</direction>"
      "<relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>"
      "<argument><name>AbsCount</name><direction>out</direction>"
      "<relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetTransportSettings</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>PlayMode</name><direction>out</direction>"
      "<relatedStateVariable>CurrentPlayMode</relatedStateVariable></argument>"
      "<argument><name>RecQualityMode</name><direction>out</direction>"
      "<relatedStateVariable>CurrentRecordQualityMode</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetMediaInfo</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>NrTracks</name><direction>out</direction>"
      "<relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
      "<argument><name>MediaDuration</name><direction>out</direction>"
      "<relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
      "<argument><name>CurrentURI</name><direction>out</direction>"
      "<relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
      "<argument><name>CurrentURIMetaData</name><direction>out</direction>"
      "<relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
      "<argument><name>NextURI</name><direction>out</direction>"
      "<relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
      "<argument><name>NextURIMetaData</name><direction>out</direction>"
      "<relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
      "<argument><name>PlayMedium</name><direction>out</direction>"
      "<relatedStateVariable>PossiblePlaybackStorageMedia</relatedStateVariable></argument>"
      "<argument><name>RecordMedium</name><direction>out</direction>"
      "<relatedStateVariable>PossibleRecordStorageMedia</relatedStateVariable></argument>"
      "<argument><name>WriteStatus</name><direction>out</direction>"
      "<relatedStateVariable>RecordWriteStatus</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetDeviceCapabilities</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>PlayMedia</name><direction>out</direction>"
      "<relatedStateVariable>PossiblePlaybackStorageMedia</relatedStateVariable></argument>"
      "<argument><name>RecMedia</name><direction>out</direction>"
      "<relatedStateVariable>PossibleRecordStorageMedia</relatedStateVariable></argument>"
      "<argument><name>RecQualityModes</name><direction>out</direction>"
      "<relatedStateVariable>PossibleRecordQualityModes</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetCurrentTransportActions</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Actions</name><direction>out</direction>"
      "<relatedStateVariable>CurrentTransportActions</relatedStateVariable></argument>"
      "</argumentList></action>"
      "</actionList>"
      "<serviceStateTable>"
      "<stateVariable sendEvents=\"no\"><name>TransportState</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>TransportStatus</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentTrack</name>"
      "<dataType>ui4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentTrackDuration</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentTrackMetaData</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>RelativeTimePosition</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>AbsoluteTimePosition</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>RelativeCounterPosition</name>"
      "<dataType>i4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>AbsoluteCounterPosition</name>"
      "<dataType>i4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentPlayMode</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentRecordQualityMode</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>NumberOfTracks</name>"
      "<dataType>ui4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentMediaDuration</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>AVTransportURI</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>NextAVTransportURI</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>NextAVTransportURIMetaData</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>PossiblePlaybackStorageMedia</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>PossibleRecordStorageMedia</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>RecordWriteStatus</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentTransportActions</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name>"
      "<dataType>ui4</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekMode</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekTarget</name>"
      "<dataType>string</dataType></stateVariable>"
      "</serviceStateTable></scpd>";
  send_xml(req, 200, scpd);
  return ESP_OK;
}

static esp_err_t rc_scpd_handler(httpd_req_t *req) {
  static const char scpd[] =
      "<?xml version=\"1.0\"?>\n"
      "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion>"
      "<actionList>"
      "<action><name>GetVolume</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>CurrentVolume</name><direction>out</direction>"
      "<relatedStateVariable>Volume</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>SetVolume</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>DesiredVolume</name><direction>in</direction>"
      "<relatedStateVariable>Volume</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetVolumeDB</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>CurrentVolumeDB</name><direction>out</direction>"
      "<relatedStateVariable>VolumeDB</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>SetVolumeDB</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>DesiredVolumeDB</name><direction>in</direction>"
      "<relatedStateVariable>VolumeDB</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetVolumeDBRange</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>MinValue</name><direction>out</direction>"
      "<relatedStateVariable>VolumeDB</relatedStateVariable></argument>"
      "<argument><name>MaxValue</name><direction>out</direction>"
      "<relatedStateVariable>VolumeDB</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>GetMute</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>CurrentMute</name><direction>out</direction>"
      "<relatedStateVariable>Mute</relatedStateVariable></argument>"
      "</argumentList></action>"
      "<action><name>SetMute</name><argumentList>"
      "<argument><name>InstanceID</name><direction>in</direction>"
      "<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
      "<argument><name>Channel</name><direction>in</direction>"
      "<relatedStateVariable>Channel</relatedStateVariable></argument>"
      "<argument><name>DesiredMute</name><direction>in</direction>"
      "<relatedStateVariable>Mute</relatedStateVariable></argument>"
      "</argumentList></action>"
      "</actionList>"
      "<serviceStateTable>"
      "<stateVariable sendEvents=\"no\"><name>Volume</name>"
      "<dataType>ui2</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>VolumeDB</name>"
      "<dataType>i2</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>Mute</name>"
      "<dataType>boolean</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>Channel</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name>"
      "<dataType>ui4</dataType></stateVariable>"
      "</serviceStateTable></scpd>";
  send_xml(req, 200, scpd);
  return ESP_OK;
}

/* ── SSDP ───────────────────────────────────────────────────────────────── */

static void ssdp_send_notify(const char *nt, const char *usn_tail) {
  if (s_ssdp_fd < 0 || !s_location[0]) {
    return;
  }
  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(SSDP_PORT);
  inet_aton(SSDP_ADDR, &dest.sin_addr);

  char usn[96];
  snprintf(usn, sizeof(usn), "%s::%s", s_uuid, usn_tail);

  char msg[512];
  int n = snprintf(msg, sizeof(msg),
                   "NOTIFY * HTTP/1.1\r\n"
                   "HOST: %s:%d\r\n"
                   "CACHE-CONTROL: max-age=1800\r\n"
                   "LOCATION: %s\r\n"
                   "NT: %s\r\n"
                   "NTS: ssdp:alive\r\n"
                   "SERVER: ESP32-S3/5.0 UPnP/1.0 ESP32-AirPlay2-DLNA/1.0\r\n"
                   "USN: %s\r\n\r\n",
                   SSDP_ADDR, SSDP_PORT, s_location, nt, usn);
  if (n > 0) {
    sendto(s_ssdp_fd, msg, (size_t)n, 0, (struct sockaddr *)&dest,
           sizeof(dest));
  }
}

static void ssdp_announce_alive(void) {
  refresh_location();
  ssdp_send_notify(URN_ROOT, URN_ROOT);
  ssdp_send_notify(URN_DEVICE, URN_DEVICE);
  ssdp_send_notify(URN_AVT, URN_AVT);
  ssdp_send_notify(URN_RC, URN_RC);
}

static void ssdp_respond_ms(char *buf, int len, struct sockaddr_in *src) {
  /* Extract the ST: line (header names are case-insensitive in HTTP). */
  char st[128] = {0};
  const char *st_line = strstr(buf, "\r\nST:");
  if (!st_line) {
    st_line = strstr(buf, "\r\nst:");
  }
  if (!st_line) {
    ESP_LOGW(TAG, "M-SEARCH without ST line, dropping");
    return;
  }
  st_line += 5;
  const char *eol = strstr(st_line, "\r\n");
  size_t n = eol ? (size_t)(eol - st_line) : strlen(st_line);
  if (n >= sizeof(st)) {
    n = sizeof(st) - 1;
  }
  memcpy(st, st_line, n);
  st[n] = 0;
  /* Trim leading/trailing whitespace: real control points send
   * "ST: ssdp:all" (space after colon), which otherwise never matches. */
  char *p = st;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  size_t pl = strlen(p);
  while (pl > 0 && (p[pl - 1] == ' ' || p[pl - 1] == '\t' || p[pl - 1] == '\r')) {
    p[--pl] = 0;
  }
  ESP_LOGI(TAG, "M-SEARCH ST='%s'", p);

  if (strcmp(p, "ssdp:all") != 0 && strcmp(p, URN_ROOT) != 0 &&
      strcmp(p, URN_DEVICE) != 0 && strcmp(p, URN_AVT) != 0 &&
      strcmp(p, URN_RC) != 0) {
    ESP_LOGI(TAG, "ST not for us, dropping");
    return; /* not for us */
  }

  refresh_location();
  if (!s_location[0]) {
    ESP_LOGW(TAG, "location empty, cannot respond");
    return;
  }

  const char *usn_tail = p;
  if (strcmp(p, "ssdp:all") == 0) {
    usn_tail = URN_DEVICE;
  }
  char usn[96];
  snprintf(usn, sizeof(usn), "%s::%s", s_uuid, usn_tail);

  char resp[512];
  int n_resp = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\n"
                        "CACHE-CONTROL: max-age=1800\r\n"
                        "DATE: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                        "EXT:\r\n"
                        "LOCATION: %s\r\n"
                        "SERVER: ESP32-S3/5.0 UPnP/1.0 ESP32-AirPlay2-DLNA/1.0\r\n"
                        "ST: %s\r\n"
                        "USN: %s\r\n"
                        "Content-Length: 0\r\n\r\n",
                        s_location, p, usn);
  if (n_resp > 0) {
    sendto(s_ssdp_fd, resp, (size_t)n_resp, 0, (struct sockaddr *)src,
           sizeof(*src));
    ESP_LOGI(TAG, "Responded to M-SEARCH (ST=%s)", st);
  }
}

static void ssdp_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "SSDP task started");
  s_ssdp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s_ssdp_fd < 0) {
    ESP_LOGE(TAG, "SSDP socket failed");
    vTaskDelete(NULL);
    return;
  }

  int on = 1;
  setsockopt(s_ssdp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = htons(SSDP_PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s_ssdp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
    ESP_LOGE(TAG, "SSDP bind failed");
    close(s_ssdp_fd);
    s_ssdp_fd = -1;
    vTaskDelete(NULL);
    return;
  }

  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  inet_aton(SSDP_ADDR, &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  int mr = setsockopt(s_ssdp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                      sizeof(mreq));
  ESP_LOGI(TAG, "SSDP mcast join rc=%d", mr);

  struct timeval tv = {2, 0};
  setsockopt(s_ssdp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ESP_LOGI(TAG, "SSDP listening on %s:%d", SSDP_ADDR, SSDP_PORT);

  /* Announce a few times on startup so control points find us quickly. */
  ssdp_announce_alive();
  ssdp_announce_alive();

  int64_t last_alive = esp_timer_get_time();

  char buf[1024];
  while (s_running) {
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    int n = recvfrom(s_ssdp_fd, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&src, &srclen);
    if (n > 0) {
      buf[n] = 0;
      ESP_LOGI(TAG, "SSDP recv %d bytes from %s:%d: %.24s", n,
               inet_ntoa(src.sin_addr), ntohs(src.sin_port), buf);
      if (strncmp(buf, "M-SEARCH", 8) == 0) {
        ssdp_respond_ms(buf, n, &src);
      }
    }
    int64_t now = esp_timer_get_time();
    if (now - last_alive > ALIVE_INTERVAL_US) {
      last_alive = now;
      ssdp_announce_alive();
      ESP_LOGI(TAG, "SSDP alive announced");
    }
  }

  close(s_ssdp_fd);
  s_ssdp_fd = -1;
  vTaskDelete(NULL);
}

esp_err_t dlna_upnp_register(httpd_handle_t server) {
  if (!server) {
    return ESP_ERR_INVALID_ARG;
  }
  make_uuid();
  refresh_location();

  httpd_uri_t u = {0};
  u.method = HTTP_GET;
  u.handler = description_handler;
  u.uri = "/description.xml";
  httpd_register_uri_handler(server, &u);

  u.handler = avt_scpd_handler;
  u.uri = "/upnp/scpd/avt.xml";
  httpd_register_uri_handler(server, &u);

  u.handler = rc_scpd_handler;
  u.uri = "/upnp/scpd/rc.xml";
  httpd_register_uri_handler(server, &u);

  u.method = HTTP_POST;
  u.handler = avt_control_handler;
  u.uri = "/upnp/control/avt";
  httpd_register_uri_handler(server, &u);

  u.handler = rc_control_handler;
  u.uri = "/upnp/control/rc";
  httpd_register_uri_handler(server, &u);

  u.handler = cm_scpd_handler;
  u.uri = "/upnp/scpd/cm.xml";
  httpd_register_uri_handler(server, &u);

  u.method = HTTP_POST;
  u.handler = cm_control_handler;
  u.uri = "/upnp/control/cm";
  httpd_register_uri_handler(server, &u);

  u.handler = event_sub_handler;
  u.uri = "/upnp/event/avt";
  httpd_register_uri_handler(server, &u);

  u.uri = "/upnp/event/rc";
  httpd_register_uri_handler(server, &u);

  u.uri = "/upnp/event/cm";
  httpd_register_uri_handler(server, &u);

  ESP_LOGI(TAG, "UPnP endpoints registered (UDN=%s)", s_uuid);
  return ESP_OK;
}

esp_err_t dlna_upnp_start_ssdp(void) {
  if (s_running) {
    return ESP_OK;
  }
  s_running = true;
  /* 4096 was too small: M-SEARCH respond path (recvfrom buf + respond +
   * location + ESP_LOG formatting) overflowed the task stack and the
   * watchdog flagged it — one of the boot-loop causes. 8192 gives margin. */
  BaseType_t ok = xTaskCreate(ssdp_task, "dlna_ssdp", 8192, NULL, 5, NULL);
  if (ok != pdPASS) {
    s_running = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void dlna_upnp_stop(void) {
  s_running = false;
}
