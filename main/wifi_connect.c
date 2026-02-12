#include "wifi_connect.h"
#include "scan_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include <string.h>

static const char *TAG = "wifi_conn";

#define MAX_STA_RETRIES 5

static wifi_conn_mode_t s_mode = WIFI_CONN_MODE_AP;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;
static int s_retry_count = 0;
static bool s_got_ip = false;

static const char *authmode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:          return "OPEN";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
        default:                      return "UNKNOWN";
    }
}

static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        if (s_retry_count < MAX_STA_RETRIES) {
            s_retry_count++;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry_count, MAX_STA_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "STA connect failed after %d retries", MAX_STA_RETRIES);
            xSemaphoreGive(s_connect_sem);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_got_ip = true;
        s_retry_count = 0;
        xSemaphoreGive(s_connect_sem);
    }
}

static void start_softap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_Locator",
            .ssid_len = strlen("ESP32_Locator"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_mode = WIFI_CONN_MODE_AP;
    ESP_LOGI(TAG, "SoftAP 'ESP32_Locator' started (open, 192.168.4.1)");
}

static bool try_sta_connect(const char *ssid, const char *pass)
{
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
    }

    s_retry_count = 0;
    s_got_ip = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    esp_wifi_connect();

    // Wait for connection or failure (timeout ~30s)
    if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(30000)) == pdTRUE && s_got_ip) {
        s_mode = WIFI_CONN_MODE_STA;
        ESP_LOGI(TAG, "STA connected to '%s'", ssid);
        return true;
    }

    ESP_LOGW(TAG, "STA connection to '%s' failed", ssid);
    esp_wifi_stop();
    return false;
}

wifi_conn_mode_t wifi_connect_init(void)
{
    s_connect_sem = xSemaphoreCreateBinary();

    // Create both netifs
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, NULL));

    // Check for stored credentials
    if (scan_store_has_wifi_creds()) {
        char ssid[33] = {0};
        char pass[65] = {0};
        scan_store_get_wifi_ssid(ssid, sizeof(ssid));
        scan_store_get_wifi_pass(pass, sizeof(pass));

        ESP_LOGI(TAG, "Found stored WiFi credentials for '%s'", ssid);
        if (try_sta_connect(ssid, pass)) {
            return WIFI_CONN_MODE_STA;
        }
        ESP_LOGW(TAG, "Stored credentials failed, falling back to SoftAP");
    } else {
        ESP_LOGI(TAG, "No stored WiFi credentials");
    }

    // Fall back to SoftAP
    start_softap();
    return WIFI_CONN_MODE_AP;
}

wifi_conn_mode_t wifi_connect_get_mode(void)
{
    return s_mode;
}

esp_err_t wifi_connect_sta(const char *ssid, const char *password)
{
    // Save credentials to NVS
    esp_err_t err = scan_store_set_wifi_ssid(ssid);
    if (err != ESP_OK) return err;
    err = scan_store_set_wifi_pass(password ? password : "");
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Credentials saved, rebooting to connect...");

    // Give time for HTTP response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;  // Never reached
}

void wifi_connect_get_ip_str(char *buf, size_t len)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = (s_mode == WIFI_CONN_MODE_STA) ? s_sta_netif : s_ap_netif;

    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(buf, "unknown", len);
    }
}

char *wifi_connect_scan_networks(void)
{
    // If in AP mode, temporarily switch to APSTA for scanning
    wifi_mode_t orig_mode;
    esp_wifi_get_mode(&orig_mode);

    if (orig_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        if (orig_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return strdup("[]");
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    uint16_t max_aps = (ap_num > 20) ? 20 : ap_num;
    wifi_ap_record_t *records = calloc(max_aps, sizeof(wifi_ap_record_t));
    if (!records) {
        if (orig_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return strdup("[]");
    }

    esp_wifi_scan_get_ap_records(&max_aps, records);

    // Build JSON array
    cJSON *arr = cJSON_CreateArray();
    // Track seen SSIDs to deduplicate
    for (uint16_t i = 0; i < max_aps; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') continue;  // skip hidden

        // Check for duplicate SSID (keep strongest signal)
        bool dup = false;
        for (int j = 0; j < cJSON_GetArraySize(arr); j++) {
            cJSON *item = cJSON_GetArrayItem(arr, j);
            cJSON *s = cJSON_GetObjectItem(item, "ssid");
            if (s && strcmp(s->valuestring, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddStringToObject(ap, "auth", authmode_str(records[i].authmode));
        cJSON_AddNumberToObject(ap, "channel", records[i].primary);
        cJSON_AddItemToArray(arr, ap);
    }

    free(records);

    // Restore mode if changed
    if (orig_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json ? json : strdup("[]");
}
