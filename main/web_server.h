#pragma once

#include "esp_http_server.h"

// Callback type for sleep request from web UI
typedef void (*sleep_callback_t)(void);

// Set the callback to invoke when /api/sleep is called.
void web_server_set_sleep_callback(sleep_callback_t cb);

// Start the web server with all locator URI handlers.
httpd_handle_t web_server_start(void);

// Stop the web server.
void web_server_stop(httpd_handle_t server);

// Start captive portal DNS server (AP mode only).
// Resolves all DNS queries to 192.168.4.1 so clients stay connected.
void web_server_start_captive_dns(void);
