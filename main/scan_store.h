#pragma once

#include "wifi_scan.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Scan blob header (3 bytes)
typedef struct __attribute__((packed)) {
    uint16_t scan_index;
    uint8_t  ap_count;
} scan_header_t;

// Initialize the locator NVS namespace. Call once at startup.
esp_err_t scan_store_init(void);

// Save a scan to NVS. Returns the assigned scan index.
esp_err_t scan_store_save(const stored_ap_t *aps, uint8_t ap_count, uint16_t *out_index);

// Load a scan from NVS by index. Caller provides buffer for aps (max_aps entries).
// Returns actual AP count in *out_ap_count.
esp_err_t scan_store_load(uint16_t index, stored_ap_t *aps, uint8_t max_aps, uint8_t *out_ap_count);

// Get only the AP count for a scan (reads just the blob header, no AP data).
esp_err_t scan_store_get_ap_count(uint16_t index, uint8_t *out_ap_count);

// Get the range of stored scan indices [*out_head .. *out_count-1]
esp_err_t scan_store_get_range(uint16_t *out_head, uint16_t *out_count);

// Delete a single scan by index
esp_err_t scan_store_delete(uint16_t index);

// Delete all scans
esp_err_t scan_store_delete_all(void);

// Get/set API key (up to 128 chars)
esp_err_t scan_store_get_api_key(char *buf, size_t buf_size);
esp_err_t scan_store_set_api_key(const char *key);

// Get/set time base for timestamp computation
esp_err_t scan_store_set_time_base(int64_t epoch, uint16_t scan_idx);
esp_err_t scan_store_get_time_base(int64_t *epoch, uint16_t *scan_idx);

// Get/set scan interval in seconds (default 60)
#define SCAN_INTERVAL_DEFAULT 60
uint16_t scan_store_get_scan_interval(void);
esp_err_t scan_store_set_scan_interval(uint16_t seconds);
