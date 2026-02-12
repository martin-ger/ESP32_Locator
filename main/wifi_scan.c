#include "wifi_scan.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <string.h>

static const char *TAG = "wifi_scan";

uint16_t wifi_scan_execute(stored_ap_t *out_aps, uint16_t max_aps)
{
    // Create default STA netif (needed for scan)
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return 0;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        esp_netif_destroy(sta_netif);
        return 0;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set STA mode failed: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        esp_netif_destroy(sta_netif);
        return 0;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        esp_netif_destroy(sta_netif);
        return 0;
    }

    // Blocking scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy(sta_netif);
        return 0;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    ESP_LOGI(TAG, "Scan found %u APs", ap_num);

    if (ap_num == 0) {
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy(sta_netif);
        return 0;
    }

    uint16_t fetch_count = (ap_num < max_aps) ? ap_num : max_aps;
    wifi_ap_record_t *ap_records = calloc(fetch_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate AP records");
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy(sta_netif);
        return 0;
    }

    esp_wifi_scan_get_ap_records(&fetch_count, ap_records);

    // Convert to stored format
    for (uint16_t i = 0; i < fetch_count; i++) {
        memcpy(out_aps[i].bssid, ap_records[i].bssid, 6);
        out_aps[i].rssi = ap_records[i].rssi;
        out_aps[i].channel = ap_records[i].primary;
        out_aps[i].authmode = (uint8_t)ap_records[i].authmode;
        size_t ssid_len = strlen((char *)ap_records[i].ssid);
        if (ssid_len > 32) ssid_len = 32;
        out_aps[i].ssid_len = (uint8_t)ssid_len;
        memset(out_aps[i].ssid, 0, 32);
        memcpy(out_aps[i].ssid, ap_records[i].ssid, ssid_len);
    }

    free(ap_records);
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy(sta_netif);

    ESP_LOGI(TAG, "Returning %u APs", fetch_count);
    return fetch_count;
}
