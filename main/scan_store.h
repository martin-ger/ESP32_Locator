#pragma once

#include "wifi_scan.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Scan blob header (11 bytes)
typedef struct __attribute__((packed)) {
    uint16_t scan_index;
    uint8_t  ap_count;
    int64_t  timestamp;   // UTC epoch seconds (from RTC)
} scan_header_t;

// Initialize the locator NVS namespace. Call once at startup.
esp_err_t scan_store_init(void);

// Save a scan to NVS with timestamp. Returns the assigned scan index.
esp_err_t scan_store_save(const stored_ap_t *aps, uint8_t ap_count, int64_t timestamp, uint16_t *out_index);

// Load a scan from NVS by index. Caller provides buffer for aps (max_aps entries).
// Returns actual AP count in *out_ap_count.
esp_err_t scan_store_load(uint16_t index, stored_ap_t *aps, uint8_t max_aps, uint8_t *out_ap_count);

// Get scan header info (ap_count + timestamp) without loading AP data.
esp_err_t scan_store_get_scan_info(uint16_t index, uint8_t *out_ap_count, int64_t *out_timestamp);

// Get the range of stored scan indices [*out_head .. *out_count-1]
esp_err_t scan_store_get_range(uint16_t *out_head, uint16_t *out_count);

// Delete a single scan by index
esp_err_t scan_store_delete(uint16_t index);

// Delete all scans
esp_err_t scan_store_delete_all(void);

// Get/set API key (up to 128 chars)
esp_err_t scan_store_get_api_key(char *buf, size_t buf_size);
esp_err_t scan_store_set_api_key(const char *key);

// Get/set scan interval in seconds (default 60)
#define SCAN_INTERVAL_DEFAULT 60
uint16_t scan_store_get_scan_interval(void);
esp_err_t scan_store_set_scan_interval(uint16_t seconds);

// Location cache per scan (stored as separate NVS blob)
typedef struct __attribute__((packed)) {
    double lat;
    double lng;
    double accuracy;
} scan_location_t;

esp_err_t scan_store_save_location(uint16_t index, double lat, double lng, double accuracy);
esp_err_t scan_store_get_location(uint16_t index, scan_location_t *out);
bool      scan_store_has_location(uint16_t index);

// WiFi credential management
esp_err_t scan_store_get_wifi_ssid(char *buf, size_t buf_size);
esp_err_t scan_store_set_wifi_ssid(const char *ssid);
esp_err_t scan_store_get_wifi_pass(char *buf, size_t buf_size);
esp_err_t scan_store_set_wifi_pass(const char *pass);
bool      scan_store_has_wifi_creds(void);
esp_err_t scan_store_clear_wifi_creds(void);
