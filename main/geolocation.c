#include "geolocation.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "geolocation";

#define MAX_RESPONSE_SIZE 1024

static char *build_request_json(const stored_ap_t *aps, uint8_t ap_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi_array = cJSON_AddArrayToObject(root, "wifiAccessPoints");

    for (uint8_t i = 0; i < ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        cJSON_AddStringToObject(ap, "macAddress", mac);
        cJSON_AddNumberToObject(ap, "signalStrength", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", aps[i].channel);
        cJSON_AddItemToArray(wifi_array, ap);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

esp_err_t geolocation_request(const char *api_key, const stored_ap_t *aps,
                              uint8_t ap_count, geolocation_result_t *result)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/geolocation/v1/geolocate?key=%s", api_key);

    char *post_data = build_request_json(aps, ap_count);
    if (!post_data) {
        ESP_LOGE(TAG, "Failed to build JSON");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Requesting geolocation with %u APs", ap_count);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    int post_len = strlen(post_data);
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(err));
        free(post_data);
        esp_http_client_cleanup(client);
        return err;
    }

    int wlen = esp_http_client_write(client, post_data, post_len);
    free(post_data);
    if (wlen < 0) {
        ESP_LOGE(TAG, "Write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Response status=%d", status);

    // Read response body
    char *resp_buf = malloc(MAX_RESPONSE_SIZE);
    if (!resp_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < MAX_RESPONSE_SIZE - 1) {
        int rlen = esp_http_client_read(client, resp_buf + total_read,
                                         MAX_RESPONSE_SIZE - 1 - total_read);
        if (rlen <= 0) break;
        total_read += rlen;
    }
    resp_buf[total_read] = 0;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Google API error %d: %s", status, resp_buf);
        free(resp_buf);
        return ESP_FAIL;
    }

    // Parse response
    cJSON *resp = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        return ESP_FAIL;
    }

    cJSON *location = cJSON_GetObjectItem(resp, "location");
    cJSON *accuracy = cJSON_GetObjectItem(resp, "accuracy");
    if (!location || !accuracy) {
        ESP_LOGE(TAG, "Missing location or accuracy in response");
        cJSON_Delete(resp);
        return ESP_FAIL;
    }

    cJSON *lat = cJSON_GetObjectItem(location, "lat");
    cJSON *lng = cJSON_GetObjectItem(location, "lng");
    if (!lat || !lng) {
        ESP_LOGE(TAG, "Missing lat/lng in response");
        cJSON_Delete(resp);
        return ESP_FAIL;
    }

    result->lat = lat->valuedouble;
    result->lng = lng->valuedouble;
    result->accuracy = accuracy->valuedouble;

    cJSON_Delete(resp);
    ESP_LOGI(TAG, "Location: lat=%.6f lng=%.6f accuracy=%.1f",
             result->lat, result->lng, result->accuracy);
    return ESP_OK;
}
