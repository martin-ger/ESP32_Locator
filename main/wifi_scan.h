#pragma once

#include "esp_wifi_types.h"
#include <stdint.h>

// Packed AP record for NVS storage (42 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;
    uint8_t  ssid_len;
    char     ssid[32];
} stored_ap_t;

// Initialize WiFi in STA mode (no connect), perform scan, deinit.
// Returns number of APs found (up to max_aps). Results written to out_aps.
// Returns 0 if no APs found or on error.
uint16_t wifi_scan_execute(stored_ap_t *out_aps, uint16_t max_aps);
