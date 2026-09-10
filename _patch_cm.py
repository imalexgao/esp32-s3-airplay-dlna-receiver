# -*- coding: utf-8 -*-
"""Add ConnectionManager (GetProtocolInfo) service to dlna_upnp.c."""
import io

p = r'main\dlna\dlna_upnp.c'
s = io.open(p, encoding='utf-8').read()

# 1. URN_CM constant
old = '#define URN_RC "urn:schemas-upnp-org:service:RenderingControl:1"'
new = ('#define URN_RC "urn:schemas-upnp-org:service:RenderingControl:1"\n'
       '#define URN_CM "urn:schemas-upnp-org:service:ConnectionManager:1"')
assert old in s
s = s.replace(old, new, 1)

# 2. cm_action() after rc_action() closing brace
marker = """  ESP_LOGW(TAG, "Unhandled RC action: %s", action);
  soap_fault(401, "Invalid Action", resp, resp_sz);
}"""
cm_block = marker + """

/* Supported sink formats advertised via GetProtocolInfo. Order matters for
 * some control points (LPCM first = default). WAV is served as LPCM;
 * OGG/AIFF are advertised as raw container types. */
#define SINK_PROTOCOL_INFO \\
  "http-get:*:audio/L16;rate=44100;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=01700000000000000000000000000000," \\
  "http-get:*:audio/L16;rate=48000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=01700000000000000000000000000000," \\
  "http-get:*:audio/mpeg:DLNA.ORG_PN=MP3," \\
  "http-get:*:audio/flac:DLNA.ORG_PN=FLAC," \\
  "http-get:*:audio/aac:DLNA.ORG_PN=AAC_ADTS," \\
  "http-get:*:audio/ogg:DLNA.ORG_PN=OGG," \\
  "http-get:*:audio/x-aiff"

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
}"""
assert marker in s
s = s.replace(marker, cm_block, 1)

# 3. description.xml: add CM service entry
old_desc = """           "<service>"
           "<serviceType>%s</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
           "<SCPDURL>/upnp/scpd/rc.xml</SCPDURL>"
           "<controlURL>/upnp/control/rc</controlURL>"
           "<eventSubURL>/upnp/event/rc</eventSubURL>"
           "</service>"
           "</serviceList>"
           "</device></root>",
           URN_DEVICE, name, s_uuid, URN_AVT, URN_RC);"""
new_desc = """           "<service>"
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
           URN_DEVICE, name, s_uuid, URN_AVT, URN_RC, URN_CM);"""
assert old_desc in s
s = s.replace(old_desc, new_desc, 1)

# 4. soap_handler: three-way dispatch
old_soap = """  char resp[2048];
  if (avt) {
    avt_action(action, body, resp, sizeof(resp));
  } else {
    rc_action(action, body, resp, sizeof(resp));
  }

  bool fault = strstr(resp, "s:Fault") != NULL;
  send_xml(req, fault ? 500 : 200, resp);
  return ESP_OK;
}

static esp_err_t avt_control_handler(httpd_req_t *req) {
  return soap_handler(req, true);
}

static esp_err_t rc_control_handler(httpd_req_t *req) {
  return soap_handler(req, false);
}"""
new_soap = """  char resp[2048];
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
  return soap_handler(req, true);
}

static esp_err_t rc_control_handler(httpd_req_t *req) {
  return soap_handler(req, false);
}

static esp_err_t cm_control_handler(httpd_req_t *req) {
  return soap_handler(req, false);
}"""
assert old_soap in s
s = s.replace(old_soap, new_soap, 1)

# 4b. soap_handler signature: bool avt -> add bool rc
old_sig = "static esp_err_t soap_handler(httpd_req_t *req, bool avt) {"
new_sig = "static esp_err_t soap_handler(httpd_req_t *req, bool avt, bool rc) {"
assert old_sig in s
s = s.replace(old_sig, new_sig, 1)

# 4c. callers pass false for rc
old_c1 = "  return soap_handler(req, true);\n}"
new_c1 = "  return soap_handler(req, true, false);\n}"
assert old_c1 in s
s = s.replace(old_c1, new_c1, 1)
old_c2 = "  return soap_handler(req, false);\n}"
new_c2 = "  return soap_handler(req, false, true);\n}"
assert old_c2 in s
s = s.replace(old_c2, new_c2, 1)

# 5. cm_scpd_handler after rc_scpd_handler (find its end via next handler)
# simpler: insert before avt_scpd_handler
scpd_anchor = "static esp_err_t avt_scpd_handler(httpd_req_t *req) {"
cm_scpd = """static esp_err_t cm_scpd_handler(httpd_req_t *req) {
  static const char scpd[] =
      "<?xml version=\\"1.0\\"?>\\n"
      "<scpd xmlns=\\"urn:schemas-upnp-org:service-1-0\\">"
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
      "<stateVariable sendEvents=\\"no\\"><name>SourceProtocolInfo</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>SinkProtocolInfo</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>CurrentConnectionIDs</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_ConnectionID</name>"
      "<dataType>ui4</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_RcsID</name>"
      "<dataType>i4</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_AVTransportID</name>"
      "<dataType>i4</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_ProtocolInfo</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_ConnectionManager</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_Direction</name>"
      "<dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\\"no\\"><name>A_ARG_TYPE_ConnectionStatus</name>"
      "<dataType>string</dataType></stateVariable>"
      "</serviceStateTable>"
      "</scpd>";
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "text/xml; charset=\\"utf-8\\"");
  httpd_resp_set_hdr(req, "EXT", "");
  httpd_resp_send(req, scpd, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

"""
assert scpd_anchor in s
s = s.replace(scpd_anchor, cm_scpd + scpd_anchor, 1)

# 6. URI registration
old_reg = """  u.handler = event_sub_handler;
  u.uri = "/upnp/event/avt";
  httpd_register_uri_handler(server, &u);

  u.uri = "/upnp/event/rc";
  httpd_register_uri_handler(server, &u);"""
new_reg = """  u.handler = cm_scpd_handler;
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
  httpd_register_uri_handler(server, &u);"""
assert old_reg in s
s = s.replace(old_reg, new_reg, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('ConnectionManager added')
