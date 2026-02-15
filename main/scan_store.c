#include "scan_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "scan_store";
static const char *NVS_NAMESPACE = "locator";
static nvs_handle_t nvs_h;

esp_err_t scan_store_init(void)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
}

static void make_scan_key(uint16_t index, char *key)
{
    snprintf(key, 7, "s%05u", index);
}

static void make_loc_key(uint16_t index, char *key)
{
    snprintf(key, 7, "l%05u", index);
}

static esp_err_t get_u16_or_default(const char *key, uint16_t *val, uint16_t def)
{
    esp_err_t err = nvs_get_u16(nvs_h, key, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *val = def;
        return ESP_OK;
    }
    return err;
}

esp_err_t scan_store_save(const stored_ap_t *aps, uint8_t ap_count, int64_t timestamp, uint16_t *out_index)
{
    uint16_t scan_count, scan_head;
    esp_err_t err;

    err = get_u16_or_default("scan_count", &scan_count, 0);
    if (err != ESP_OK) return err;
    err = get_u16_or_default("scan_head", &scan_head, 0);
    if (err != ESP_OK) return err;

    // Evict oldest if at max capacity
    uint16_t stored = scan_count - scan_head;
    if (stored >= CONFIG_LOCATOR_MAX_STORED_SCANS) {
        char key[7];
        make_scan_key(scan_head, key);
        nvs_erase_key(nvs_h, key);
        make_loc_key(scan_head, key);
        nvs_erase_key(nvs_h, key);  // also evict cached location
        scan_head++;
        err = nvs_set_u16(nvs_h, "scan_head", scan_head);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "Evicted oldest scan, head now %u", scan_head);
    }

    // Build blob: header + AP records
    size_t blob_size = sizeof(scan_header_t) + ap_count * sizeof(stored_ap_t);
    uint8_t *blob = malloc(blob_size);
    if (!blob) return ESP_ERR_NO_MEM;

    scan_header_t *hdr = (scan_header_t *)blob;
    hdr->scan_index = scan_count;
    hdr->ap_count = ap_count;
    hdr->timestamp = timestamp;
    memcpy(blob + sizeof(scan_header_t), aps, ap_count * sizeof(stored_ap_t));

    char key[7];
    make_scan_key(scan_count, key);
    err = nvs_set_blob(nvs_h, key, blob, blob_size);
    free(blob);
    if (err != ESP_OK) return err;

    // Update scan_count
    scan_count++;
    err = nvs_set_u16(nvs_h, "scan_count", scan_count);
    if (err != ESP_OK) return err;

    err = nvs_commit(nvs_h);
    if (err != ESP_OK) return err;

    if (out_index) *out_index = scan_count - 1;
    ESP_LOGI(TAG, "Saved scan %u with %u APs (%u bytes)", scan_count - 1, ap_count, (unsigned)blob_size);
    return ESP_OK;
}

esp_err_t scan_store_load(uint16_t index, stored_ap_t *aps, uint8_t max_aps, uint8_t *out_ap_count)
{
    char key[7];
    make_scan_key(index, key);

    // First get the blob size
    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs_h, key, NULL, &blob_size);
    if (err != ESP_OK) return err;

    uint8_t *blob = malloc(blob_size);
    if (!blob) return ESP_ERR_NO_MEM;

    err = nvs_get_blob(nvs_h, key, blob, &blob_size);
    if (err != ESP_OK) {
        free(blob);
        return err;
    }

    scan_header_t *hdr = (scan_header_t *)blob;
    uint8_t ap_count = hdr->ap_count;
    if (ap_count > max_aps) ap_count = max_aps;

    memcpy(aps, blob + sizeof(scan_header_t), ap_count * sizeof(stored_ap_t));
    *out_ap_count = ap_count;

    free(blob);
    return ESP_OK;
}

esp_err_t scan_store_get_scan_info(uint16_t index, uint8_t *out_ap_count, int64_t *out_timestamp)
{
    char key[7];
    make_scan_key(index, key);

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs_h, key, NULL, &blob_size);
    if (err != ESP_OK) return err;

    if (blob_size < sizeof(scan_header_t)) return ESP_ERR_INVALID_SIZE;

    uint8_t *blob = malloc(blob_size);
    if (!blob) return ESP_ERR_NO_MEM;

    err = nvs_get_blob(nvs_h, key, blob, &blob_size);
    if (err == ESP_OK) {
        scan_header_t hdr;
        memcpy(&hdr, blob, sizeof(scan_header_t));
        if (out_ap_count) *out_ap_count = hdr.ap_count;
        if (out_timestamp) *out_timestamp = hdr.timestamp;
    }
    free(blob);
    return err;
}

esp_err_t scan_store_get_range(uint16_t *out_head, uint16_t *out_count)
{
    esp_err_t err;
    err = get_u16_or_default("scan_head", out_head, 0);
    if (err != ESP_OK) return err;
    err = get_u16_or_default("scan_count", out_count, 0);
    return err;
}

esp_err_t scan_store_delete(uint16_t index)
{
    char key[7];
    make_scan_key(index, key);
    esp_err_t err = nvs_erase_key(nvs_h, key);
    if (err != ESP_OK) return err;
    make_loc_key(index, key);
    nvs_erase_key(nvs_h, key);  // also delete cached location
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_delete_all(void)
{
    uint16_t head, count;
    esp_err_t err = scan_store_get_range(&head, &count);
    if (err != ESP_OK) return err;

    for (uint16_t i = head; i < count; i++) {
        char key[7];
        make_scan_key(i, key);
        nvs_erase_key(nvs_h, key);  // Ignore errors for missing keys
        make_loc_key(i, key);
        nvs_erase_key(nvs_h, key);  // also delete cached locations
    }

    // Reset counters
    err = nvs_set_u16(nvs_h, "scan_count", 0);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(nvs_h, "scan_head", 0);
    if (err != ESP_OK) return err;

    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_api_key(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "api_key", buf, &buf_size);
}

esp_err_t scan_store_set_api_key(const char *key)
{
    esp_err_t err = nvs_set_str(nvs_h, "api_key", key);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

uint16_t scan_store_get_scan_interval(void)
{
    uint16_t val;
    esp_err_t err = nvs_get_u16(nvs_h, "scan_ivl", &val);
    if (err != ESP_OK) return SCAN_INTERVAL_DEFAULT;
    return val;
}

esp_err_t scan_store_set_scan_interval(uint16_t seconds)
{
    esp_err_t err = nvs_set_u16(nvs_h, "scan_ivl", seconds);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_web_password(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "web_pass", buf, &buf_size);
}

esp_err_t scan_store_set_web_password(const char *pass)
{
    esp_err_t err;
    if (pass[0] == '\0') {
        err = nvs_erase_key(nvs_h, "web_pass");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(nvs_h, "web_pass", pass);
    }
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_save_location(uint16_t index, double lat, double lng, double accuracy)
{
    char key[7];
    make_loc_key(index, key);
    scan_location_t loc = { .lat = lat, .lng = lng, .accuracy = accuracy };
    esp_err_t err = nvs_set_blob(nvs_h, key, &loc, sizeof(loc));
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_location(uint16_t index, scan_location_t *out)
{
    char key[7];
    make_loc_key(index, key);
    size_t size = sizeof(scan_location_t);
    return nvs_get_blob(nvs_h, key, out, &size);
}

bool scan_store_has_location(uint16_t index)
{
    char key[7];
    make_loc_key(index, key);
    size_t size = 0;
    return nvs_get_blob(nvs_h, key, NULL, &size) == ESP_OK;
}

esp_err_t scan_store_get_wifi_ssid(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "wifi_ssid", buf, &buf_size);
}

esp_err_t scan_store_set_wifi_ssid(const char *ssid)
{
    esp_err_t err = nvs_set_str(nvs_h, "wifi_ssid", ssid);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_wifi_pass(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "wifi_pass", buf, &buf_size);
}

esp_err_t scan_store_set_wifi_pass(const char *pass)
{
    esp_err_t err = nvs_set_str(nvs_h, "wifi_pass", pass);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

bool scan_store_has_wifi_creds(void)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs_h, "wifi_ssid", NULL, &len);
    return (err == ESP_OK && len > 1);
}

esp_err_t scan_store_clear_wifi_creds(void)
{
    nvs_erase_key(nvs_h, "wifi_ssid");
    nvs_erase_key(nvs_h, "wifi_pass");
    return nvs_commit(nvs_h);
}

// --- MQTT configuration ---

static esp_err_t set_str_or_erase(const char *key, const char *val)
{
    esp_err_t err;
    if (val[0] == '\0') {
        err = nvs_erase_key(nvs_h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(nvs_h, key, val);
    }
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_mqtt_url_last(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "mqtt_url_l", buf, &buf_size);
}

esp_err_t scan_store_set_mqtt_url_last(const char *url)
{
    return set_str_or_erase("mqtt_url_l", url);
}

esp_err_t scan_store_get_mqtt_url_all(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "mqtt_url_a", buf, &buf_size);
}

esp_err_t scan_store_set_mqtt_url_all(const char *url)
{
    return set_str_or_erase("mqtt_url_a", url);
}

uint16_t scan_store_get_mqtt_wait_cycles(void)
{
    uint16_t val;
    esp_err_t err = nvs_get_u16(nvs_h, "mqtt_wait", &val);
    if (err != ESP_OK) return 0;
    return val;
}

esp_err_t scan_store_set_mqtt_wait_cycles(uint16_t cycles)
{
    esp_err_t err = nvs_set_u16(nvs_h, "mqtt_wait", cycles);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_mqtt_client_id(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "mqtt_cid", buf, &buf_size);
}

esp_err_t scan_store_set_mqtt_client_id(const char *id)
{
    return set_str_or_erase("mqtt_cid", id);
}

esp_err_t scan_store_get_mqtt_username(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "mqtt_user", buf, &buf_size);
}

esp_err_t scan_store_set_mqtt_username(const char *user)
{
    return set_str_or_erase("mqtt_user", user);
}

esp_err_t scan_store_get_mqtt_password(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "mqtt_pass", buf, &buf_size);
}

esp_err_t scan_store_set_mqtt_password(const char *pass)
{
    return set_str_or_erase("mqtt_pass", pass);
}

uint16_t scan_store_get_mqtt_cycle_counter(void)
{
    uint16_t val;
    esp_err_t err = nvs_get_u16(nvs_h, "mqtt_cycle", &val);
    if (err != ESP_OK) return 0;
    return val;
}

esp_err_t scan_store_set_mqtt_cycle_counter(uint16_t count)
{
    esp_err_t err = nvs_set_u16(nvs_h, "mqtt_cycle", count);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

// --- Open WiFi mode/URL ---

uint8_t scan_store_get_open_wifi_mode(void)
{
    uint8_t val;
    esp_err_t err = nvs_get_u8(nvs_h, "ow_mode", &val);
    if (err != ESP_OK) return OPEN_WIFI_OFF;
    return val;
}

esp_err_t scan_store_set_open_wifi_mode(uint8_t mode)
{
    esp_err_t err = nvs_set_u8(nvs_h, "ow_mode", mode);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

// --- Boot mode ---

uint8_t scan_store_get_boot_mode(void)
{
    uint8_t val;
    esp_err_t err = nvs_get_u8(nvs_h, "boot_mode", &val);
    if (err != ESP_OK) return BOOT_MODE_WEB;
    return val;
}

esp_err_t scan_store_set_boot_mode(uint8_t mode)
{
    esp_err_t err = nvs_set_u8(nvs_h, "boot_mode", mode);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_open_wifi_url(char *buf, size_t buf_size)
{
    return nvs_get_str(nvs_h, "ow_url", buf, &buf_size);
}

esp_err_t scan_store_set_open_wifi_url(const char *url)
{
    esp_err_t err;
    if (url[0] == '\0') {
        err = nvs_erase_key(nvs_h, "ow_url");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(nvs_h, "ow_url", url);
    }
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

// --- Open WiFi SSID blocklist (FIFO ring buffer) ---

static void make_bl_key(uint8_t slot, char *key)
{
    snprintf(key, 6, "bl%u", slot);
}

static esp_err_t get_u8_or_default(const char *key, uint8_t *val, uint8_t def)
{
    esp_err_t err = nvs_get_u8(nvs_h, key, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *val = def;
        return ESP_OK;
    }
    return err;
}

bool scan_store_blocklist_contains(const char *ssid)
{
    uint8_t count, head;
    if (get_u8_or_default("bl_count", &count, 0) != ESP_OK) return false;
    if (get_u8_or_default("bl_head", &head, 0) != ESP_OK) return false;
    if (count == 0) return false;

    uint8_t n = (count < BLOCKLIST_SIZE) ? count : BLOCKLIST_SIZE;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t slot = (head + i) % BLOCKLIST_SIZE;
        char key[6];
        make_bl_key(slot, key);
        char buf[33] = {0};
        size_t buf_size = sizeof(buf);
        if (nvs_get_str(nvs_h, key, buf, &buf_size) == ESP_OK) {
            if (strcmp(buf, ssid) == 0) return true;
        }
    }
    return false;
}

esp_err_t scan_store_blocklist_add(const char *ssid)
{
    // Deduplicate
    if (scan_store_blocklist_contains(ssid)) return ESP_OK;

    uint8_t count, head;
    esp_err_t err;
    err = get_u8_or_default("bl_count", &count, 0);
    if (err != ESP_OK) return err;
    err = get_u8_or_default("bl_head", &head, 0);
    if (err != ESP_OK) return err;

    uint8_t slot;
    if (count < BLOCKLIST_SIZE) {
        slot = (head + count) % BLOCKLIST_SIZE;
        count++;
    } else {
        // Evict oldest (head), write to that slot, advance head
        slot = head;
        head = (head + 1) % BLOCKLIST_SIZE;
        err = nvs_set_u8(nvs_h, "bl_head", head);
        if (err != ESP_OK) return err;
    }

    char key[6];
    make_bl_key(slot, key);
    err = nvs_set_str(nvs_h, key, ssid);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(nvs_h, "bl_count", count);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Blocklisted SSID '%s' (slot %u)", ssid, slot);
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_blocklist_delete(const char *ssid)
{
    uint8_t count, head;
    esp_err_t err;
    err = get_u8_or_default("bl_count", &count, 0);
    if (err != ESP_OK) return err;
    err = get_u8_or_default("bl_head", &head, 0);
    if (err != ESP_OK) return err;
    if (count == 0) return ESP_ERR_NOT_FOUND;

    uint8_t n = (count < BLOCKLIST_SIZE) ? count : BLOCKLIST_SIZE;
    char kept[BLOCKLIST_SIZE][33];
    int kept_count = 0;
    bool found = false;

    for (uint8_t i = 0; i < n; i++) {
        uint8_t slot = (head + i) % BLOCKLIST_SIZE;
        char key[6];
        make_bl_key(slot, key);
        char buf[33] = {0};
        size_t buf_size = sizeof(buf);
        if (nvs_get_str(nvs_h, key, buf, &buf_size) == ESP_OK) {
            if (strcmp(buf, ssid) == 0) {
                found = true;
            } else {
                strncpy(kept[kept_count], buf, 33);
                kept[kept_count][32] = '\0';
                kept_count++;
            }
        }
    }

    if (!found) return ESP_ERR_NOT_FOUND;

    // Clear all slots and rewrite
    for (uint8_t i = 0; i < BLOCKLIST_SIZE; i++) {
        char key[6];
        make_bl_key(i, key);
        nvs_erase_key(nvs_h, key);
    }
    for (int i = 0; i < kept_count; i++) {
        char key[6];
        make_bl_key(i, key);
        nvs_set_str(nvs_h, key, kept[i]);
    }

    nvs_set_u8(nvs_h, "bl_head", 0);
    nvs_set_u8(nvs_h, "bl_count", (uint8_t)kept_count);
    ESP_LOGI(TAG, "Removed '%s' from blocklist (%d remaining)", ssid, kept_count);
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_blocklist_clear(void)
{
    uint8_t count;
    get_u8_or_default("bl_count", &count, 0);

    for (uint8_t i = 0; i < BLOCKLIST_SIZE; i++) {
        char key[6];
        make_bl_key(i, key);
        nvs_erase_key(nvs_h, key);
    }
    nvs_erase_key(nvs_h, "bl_count");
    nvs_erase_key(nvs_h, "bl_head");
    return nvs_commit(nvs_h);
}

int scan_store_blocklist_list(char ssids[][33], int max_entries)
{
    uint8_t count, head;
    if (get_u8_or_default("bl_count", &count, 0) != ESP_OK) return 0;
    if (get_u8_or_default("bl_head", &head, 0) != ESP_OK) return 0;
    if (count == 0) return 0;

    uint8_t n = (count < BLOCKLIST_SIZE) ? count : BLOCKLIST_SIZE;
    int result = 0;
    for (uint8_t i = 0; i < n && result < max_entries; i++) {
        uint8_t slot = (head + i) % BLOCKLIST_SIZE;
        char key[6];
        make_bl_key(slot, key);
        size_t buf_size = 33;
        if (nvs_get_str(nvs_h, key, ssids[result], &buf_size) == ESP_OK) {
            result++;
        }
    }
    return result;
}
