#include "mqtt_publish.h"
#include "scan_store.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "mqtt_pub";

static SemaphoreHandle_t s_connected_sem = NULL;
static SemaphoreHandle_t s_published_sem = NULL;
static volatile bool s_is_connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_is_connected = true;
            xSemaphoreGive(s_connected_sem);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_is_connected = false;
            xSemaphoreGive(s_connected_sem);
            break;
        case MQTT_EVENT_PUBLISHED:
            xSemaphoreGive(s_published_sem);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT error type: %d", event->error_handle->error_type);
            break;
        default:
            break;
    }
}

// Parse "mqtt://host:port/topic/path" into broker URI and topic.
// broker_uri gets "mqtt://host:port", topic gets "topic/path".
static bool parse_mqtt_url(const char *url, char *broker_uri, size_t broker_size,
                           char *topic, size_t topic_size)
{
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return false;

    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');

    if (!path_start || path_start[1] == '\0') {
        ESP_LOGW(TAG, "MQTT URL has no topic path: %s", url);
        return false;
    }

    size_t broker_len = path_start - url;
    if (broker_len >= broker_size) return false;
    memcpy(broker_uri, url, broker_len);
    broker_uri[broker_len] = '\0';

    const char *topic_str = path_start + 1;
    if (strlen(topic_str) >= topic_size) return false;
    strcpy(topic, topic_str);

    return true;
}

static const char *authmode_str(uint8_t mode)
{
    switch (mode) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA_PSK";
        case 3: return "WPA2_PSK";
        case 4: return "WPA_WPA2_PSK";
        case 5: return "WPA2_ENTERPRISE";
        case 6: return "WPA3_PSK";
        case 7: return "WPA2_WPA3_PSK";
        default: return "UNKNOWN";
    }
}

static char *build_scan_json(uint16_t id)
{
    stored_ap_t aps[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
    uint8_t ap_count = 0;
    if (scan_store_load(id, aps, CONFIG_LOCATOR_MAX_APS_PER_SCAN, &ap_count) != ESP_OK) {
        return NULL;
    }

    int64_t timestamp = 0;
    scan_store_get_scan_info(id, NULL, &timestamp);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "timestamp", (double)timestamp);
    cJSON *arr = cJSON_AddArrayToObject(root, "aps");

    for (uint8_t i = 0; i < ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        char ssid[33];
        memcpy(ssid, aps[i].ssid, aps[i].ssid_len);
        ssid[aps[i].ssid_len] = '\0';
        cJSON_AddStringToObject(ap, "ssid", ssid);

        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        cJSON_AddStringToObject(ap, "bssid", mac);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", aps[i].channel);
        cJSON_AddStringToObject(ap, "auth", authmode_str(aps[i].authmode));
        cJSON_AddItemToArray(arr, ap);
    }

    scan_location_t loc;
    if (scan_store_get_location(id, &loc) == ESP_OK) {
        cJSON *location = cJSON_AddObjectToObject(root, "location");
        cJSON_AddNumberToObject(location, "lat", loc.lat);
        cJSON_AddNumberToObject(location, "lng", loc.lng);
        cJSON_AddNumberToObject(location, "accuracy", loc.accuracy);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_mqtt_client_handle_t connect_mqtt(const char *broker_uri)
{
    char client_id[65] = {0};
    char username[65] = {0};
    char password[65] = {0};

    scan_store_get_mqtt_client_id(client_id, sizeof(client_id));
    scan_store_get_mqtt_username(username, sizeof(username));
    scan_store_get_mqtt_password(password, sizeof(password));

    ESP_LOGI(TAG, "MQTT credentials: client_id='%s' user='%s' pass=%s",
             client_id[0] ? client_id : "(none)",
             username[0] ? username : "(none)",
             password[0] ? "(set)" : "(none)");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
    };

    if (client_id[0]) mqtt_cfg.credentials.client_id = client_id;
    if (username[0])  mqtt_cfg.credentials.username = username;
    if (password[0])  mqtt_cfg.credentials.authentication.password = password;

    s_connected_sem = xSemaphoreCreateBinary();
    s_published_sem = xSemaphoreCreateBinary();
    s_is_connected = false;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        vSemaphoreDelete(s_connected_sem);
        vSemaphoreDelete(s_published_sem);
        s_connected_sem = NULL;
        s_published_sem = NULL;
        return NULL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    bool sem_ok = xSemaphoreTake(s_connected_sem, pdMS_TO_TICKS(15000)) == pdTRUE;
    if (!sem_ok || !s_is_connected) {
        ESP_LOGW(TAG, "MQTT connection to %s failed (%s)", broker_uri,
                 sem_ok ? "rejected" : "timed out");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        vSemaphoreDelete(s_connected_sem);
        vSemaphoreDelete(s_published_sem);
        s_connected_sem = NULL;
        s_published_sem = NULL;
        return NULL;
    }

    ESP_LOGI(TAG, "Connected to %s", broker_uri);
    return client;
}

static void disconnect_mqtt(esp_mqtt_client_handle_t client)
{
    esp_mqtt_client_disconnect(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);

    if (s_connected_sem) { vSemaphoreDelete(s_connected_sem); s_connected_sem = NULL; }
    if (s_published_sem) { vSemaphoreDelete(s_published_sem); s_published_sem = NULL; }
    s_is_connected = false;
}

static bool publish_and_wait(esp_mqtt_client_handle_t client,
                             const char *topic, const char *data, int len)
{
    int msg_id = esp_mqtt_client_publish(client, topic, data, len, 0, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish to '%s' failed", topic);
        return false;
    }

    // QoS 0: no broker acknowledgement, brief delay to let it send
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Published to '%s' (%d bytes)", topic, len);
    return true;
}

esp_err_t mqtt_publish_scans(void)
{
    char url_last[257] = {0};
    char url_all[257] = {0};

    bool has_last = (scan_store_get_mqtt_url_last(url_last, sizeof(url_last)) == ESP_OK && url_last[0]);
    bool has_all  = (scan_store_get_mqtt_url_all(url_all, sizeof(url_all)) == ESP_OK && url_all[0]);

    if (!has_last && !has_all) {
        ESP_LOGI(TAG, "No MQTT URLs configured, skipping");
        return ESP_OK;
    }

    // Check if "all" publish is due this cycle
    bool do_all = false;
    if (has_all) {
        uint16_t wait = scan_store_get_mqtt_wait_cycles();
        uint16_t counter = scan_store_get_mqtt_cycle_counter();
        counter++;
        if (wait == 0 || counter >= wait) {
            do_all = true;
            counter = 0;
        }
        scan_store_set_mqtt_cycle_counter(counter);
    }

    // Parse URLs
    char broker_last[257] = {0}, topic_last[128] = {0};
    char broker_all[257] = {0}, topic_all[128] = {0};

    if (has_last && !parse_mqtt_url(url_last, broker_last, sizeof(broker_last),
                                     topic_last, sizeof(topic_last))) {
        ESP_LOGW(TAG, "Invalid MQTT URL for last scan: %s", url_last);
        has_last = false;
    }

    if (do_all && !parse_mqtt_url(url_all, broker_all, sizeof(broker_all),
                                   topic_all, sizeof(topic_all))) {
        ESP_LOGW(TAG, "Invalid MQTT URL for all scans: %s", url_all);
        do_all = false;
    }

    if (!has_last && !do_all) return ESP_OK;

    // Connect to broker (reuse connection if both URLs share same broker)
    const char *broker_uri = has_last ? broker_last : broker_all;
    esp_mqtt_client_handle_t client = connect_mqtt(broker_uri);
    if (!client) return ESP_FAIL;

    // Publish latest scan
    if (has_last) {
        uint16_t head, count;
        if (scan_store_get_range(&head, &count) == ESP_OK && count > head) {
            uint16_t latest_id = count - 1;
            char *json = build_scan_json(latest_id);
            if (json) {
                publish_and_wait(client, topic_last, json, strlen(json));
                free(json);
            }
        }
    }

    // Publish all scans (same connection if same broker, reconnect otherwise)
    if (do_all) {
        if (has_last && strcmp(broker_last, broker_all) != 0) {
            disconnect_mqtt(client);
            client = connect_mqtt(broker_all);
            if (!client) return ESP_FAIL;
        }

        uint16_t head, count;
        if (scan_store_get_range(&head, &count) == ESP_OK && count > head) {
            cJSON *arr = cJSON_CreateArray();
            for (uint16_t i = head; i < count; i++) {
                stored_ap_t aps[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
                uint8_t ap_count = 0;
                if (scan_store_load(i, aps, CONFIG_LOCATOR_MAX_APS_PER_SCAN, &ap_count) != ESP_OK)
                    continue;

                int64_t timestamp = 0;
                scan_store_get_scan_info(i, NULL, &timestamp);

                cJSON *scan = cJSON_CreateObject();
                if (!scan) break;  // out of memory
                cJSON_AddNumberToObject(scan, "id", i);
                cJSON_AddNumberToObject(scan, "timestamp", (double)timestamp);
                cJSON *ap_arr = cJSON_AddArrayToObject(scan, "aps");

                for (uint8_t j = 0; j < ap_count; j++) {
                    cJSON *ap = cJSON_CreateObject();
                    if (!ap) break;
                    char ssid[33];
                    memcpy(ssid, aps[j].ssid, aps[j].ssid_len);
                    ssid[aps[j].ssid_len] = '\0';
                    cJSON_AddStringToObject(ap, "ssid", ssid);

                    char mac[18];
                    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                             aps[j].bssid[0], aps[j].bssid[1], aps[j].bssid[2],
                             aps[j].bssid[3], aps[j].bssid[4], aps[j].bssid[5]);
                    cJSON_AddStringToObject(ap, "bssid", mac);
                    cJSON_AddNumberToObject(ap, "rssi", aps[j].rssi);
                    cJSON_AddNumberToObject(ap, "channel", aps[j].channel);
                    cJSON_AddStringToObject(ap, "auth", authmode_str(aps[j].authmode));
                    cJSON_AddItemToArray(ap_arr, ap);
                }

                scan_location_t loc;
                if (scan_store_get_location(i, &loc) == ESP_OK) {
                    cJSON *location = cJSON_AddObjectToObject(scan, "location");
                    cJSON_AddNumberToObject(location, "lat", loc.lat);
                    cJSON_AddNumberToObject(location, "lng", loc.lng);
                    cJSON_AddNumberToObject(location, "accuracy", loc.accuracy);
                }

                cJSON_AddItemToArray(arr, scan);
            }

            char *json = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            if (json) {
                publish_and_wait(client, topic_all, json, strlen(json));
                free(json);
            } else {
                ESP_LOGW(TAG, "Failed to serialize all scans (out of memory?)");
            }
        }
    }

    disconnect_mqtt(client);
    ESP_LOGI(TAG, "MQTT publish complete");
    return ESP_OK;
}
