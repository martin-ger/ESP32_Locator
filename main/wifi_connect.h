#pragma once

#include "esp_err.h"
#include <stddef.h>

typedef enum {
    WIFI_CONN_MODE_STA,
    WIFI_CONN_MODE_AP,
} wifi_conn_mode_t;

// Initialize WiFi: try STA with stored credentials, fall back to SoftAP.
// Must call esp_netif_init() and esp_event_loop_create_default() before this.
wifi_conn_mode_t wifi_connect_init(void);

// Get current connection mode.
wifi_conn_mode_t wifi_connect_get_mode(void);

// Save credentials, attempt STA connection. Reboots on success, returns to AP on failure.
esp_err_t wifi_connect_sta(const char *ssid, const char *password);

// Get current IP address as string (e.g. "192.168.4.1" or DHCP IP).
void wifi_connect_get_ip_str(char *buf, size_t len);

// Scan for nearby WiFi networks. Returns malloc'd JSON string (caller must free).
// Format: [{"ssid":"...","rssi":-50,"auth":"WPA2_PSK","channel":6}, ...]
char *wifi_connect_scan_networks(void);
