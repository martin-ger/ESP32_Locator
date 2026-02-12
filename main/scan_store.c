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

static esp_err_t get_u16_or_default(const char *key, uint16_t *val, uint16_t def)
{
    esp_err_t err = nvs_get_u16(nvs_h, key, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *val = def;
        return ESP_OK;
    }
    return err;
}

esp_err_t scan_store_save(const stored_ap_t *aps, uint8_t ap_count, uint16_t *out_index)
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

esp_err_t scan_store_get_ap_count(uint16_t index, uint8_t *out_ap_count)
{
    char key[7];
    make_scan_key(index, key);

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs_h, key, NULL, &blob_size);
    if (err != ESP_OK) return err;

    if (blob_size < sizeof(scan_header_t)) return ESP_ERR_INVALID_SIZE;

    scan_header_t hdr;
    size_t hdr_size = sizeof(scan_header_t);
    // NVS reads the full blob even if we pass a smaller buffer,
    // but we only need the header — read the minimum
    uint8_t *blob = malloc(blob_size);
    if (!blob) return ESP_ERR_NO_MEM;

    err = nvs_get_blob(nvs_h, key, blob, &blob_size);
    if (err == ESP_OK) {
        memcpy(&hdr, blob, sizeof(scan_header_t));
        *out_ap_count = hdr.ap_count;
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

esp_err_t scan_store_set_time_base(int64_t epoch, uint16_t scan_idx)
{
    esp_err_t err = nvs_set_i64(nvs_h, "time_base", epoch);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(nvs_h, "time_base_idx", scan_idx);
    if (err != ESP_OK) return err;
    return nvs_commit(nvs_h);
}

esp_err_t scan_store_get_time_base(int64_t *epoch, uint16_t *scan_idx)
{
    esp_err_t err = nvs_get_i64(nvs_h, "time_base", epoch);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *epoch = 0;
        *scan_idx = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    return nvs_get_u16(nvs_h, "time_base_idx", scan_idx);
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
