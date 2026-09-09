#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * DLNA/UPnP AV MediaRenderer protocol layer.
 *
 * Provides:
 *  - SSDP (UDP 1900) discovery: responds to M-SEARCH, announces alive.
 *  - HTTP endpoints on the shared web server:
 *      GET  /description.xml          device description
 *      GET  /upnp/scpd/avt.xml        AVTransport SCPD
 *      GET  /upnp/scpd/rc.xml         RenderingControl SCPD
 *      POST /upnp/control/avt         AVTransport SOAP control
 *      POST /upnp/control/rc          RenderingControl SOAP control
 *      POST /upnp/event/avt|rc        GENA subscribe (ack, no push yet)
 */

/**
 * Register the DLNA HTTP endpoints on the running web server.
 * Call from web_server_start().
 */
esp_err_t dlna_upnp_register(httpd_handle_t server);

/**
 * Start the SSDP discovery task (UDP 1900).
 */
esp_err_t dlna_upnp_start_ssdp(void);

/**
 * Stop the SSDP task.
 */
void dlna_upnp_stop(void);
