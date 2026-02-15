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

// Web server password (up to 64 chars). Empty/not-set = auth disabled.
esp_err_t scan_store_get_web_password(char *buf, size_t buf_size);
esp_err_t scan_store_set_web_password(const char *pass);

// WiFi credential management
esp_err_t scan_store_get_wifi_ssid(char *buf, size_t buf_size);
esp_err_t scan_store_set_wifi_ssid(const char *ssid);
esp_err_t scan_store_get_wifi_pass(char *buf, size_t buf_size);
esp_err_t scan_store_set_wifi_pass(const char *pass);
bool      scan_store_has_wifi_creds(void);
esp_err_t scan_store_clear_wifi_creds(void);

// Default boot mode: 0=web server, 1=scan mode
#define BOOT_MODE_WEB   0
#define BOOT_MODE_SCAN  1
uint8_t   scan_store_get_boot_mode(void);
esp_err_t scan_store_set_boot_mode(uint8_t mode);

// Open WiFi mode: 0=off, 1=sync only, 2=request+sync
#define OPEN_WIFI_OFF  0
#define OPEN_WIFI_SYNC 1
#define OPEN_WIFI_REQ  2
uint8_t   scan_store_get_open_wifi_mode(void);
esp_err_t scan_store_set_open_wifi_mode(uint8_t mode);
esp_err_t scan_store_get_open_wifi_url(char *buf, size_t buf_size);
esp_err_t scan_store_set_open_wifi_url(const char *url);

// MQTT configuration
esp_err_t scan_store_get_mqtt_url_last(char *buf, size_t buf_size);
esp_err_t scan_store_set_mqtt_url_last(const char *url);
esp_err_t scan_store_get_mqtt_url_all(char *buf, size_t buf_size);
esp_err_t scan_store_set_mqtt_url_all(const char *url);
uint16_t  scan_store_get_mqtt_wait_cycles(void);
esp_err_t scan_store_set_mqtt_wait_cycles(uint16_t cycles);
esp_err_t scan_store_get_mqtt_client_id(char *buf, size_t buf_size);
esp_err_t scan_store_set_mqtt_client_id(const char *id);
esp_err_t scan_store_get_mqtt_username(char *buf, size_t buf_size);
esp_err_t scan_store_set_mqtt_username(const char *user);
esp_err_t scan_store_get_mqtt_password(char *buf, size_t buf_size);
esp_err_t scan_store_set_mqtt_password(const char *pass);
uint16_t  scan_store_get_mqtt_cycle_counter(void);
esp_err_t scan_store_set_mqtt_cycle_counter(uint16_t count);

// Open WiFi SSID blocklist (FIFO ring buffer, 10 slots)
#define BLOCKLIST_SIZE 10
bool      scan_store_blocklist_contains(const char *ssid);
esp_err_t scan_store_blocklist_add(const char *ssid);
esp_err_t scan_store_blocklist_delete(const char *ssid);
esp_err_t scan_store_blocklist_clear(void);
int       scan_store_blocklist_list(char ssids[][33], int max_entries);
