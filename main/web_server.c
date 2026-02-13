#include "web_server.h"
#include "scan_store.h"
#include "geolocation.h"
#include "wifi_connect.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <sys/param.h>

static const char *TAG = "web_server";

static sleep_callback_t s_sleep_cb = NULL;

void web_server_set_sleep_callback(sleep_callback_t cb)
{
    s_sleep_cb = cb;
}

// Embedded HTML file
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Embedded favicon
extern const uint8_t favicon_png_start[] asm("_binary_favicon_png_start");
extern const uint8_t favicon_png_end[]   asm("_binary_favicon_png_end");

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

// Set CORS headers so external clients (e.g. locator.html) can access the API
static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// Handle CORS preflight for any /api/* route
static esp_err_t api_options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// GET / — serve index.html
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

// GET /favicon.ico — serve favicon
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    httpd_resp_send(req, (const char *)favicon_png_start,
                    favicon_png_end - favicon_png_start);
    return ESP_OK;
}

// Compute symmetric difference between two BSSID sets
static int bssid_diff(const uint8_t prev[][6], uint8_t prev_n,
                      const uint8_t curr[][6], uint8_t curr_n)
{
    int diffs = 0;
    // MACs in curr not in prev
    for (uint8_t i = 0; i < curr_n; i++) {
        bool found = false;
        for (uint8_t j = 0; j < prev_n; j++) {
            if (memcmp(curr[i], prev[j], 6) == 0) { found = true; break; }
        }
        if (!found) diffs++;
    }
    // MACs in prev not in curr
    for (uint8_t i = 0; i < prev_n; i++) {
        bool found = false;
        for (uint8_t j = 0; j < curr_n; j++) {
            if (memcmp(prev[i], curr[j], 6) == 0) { found = true; break; }
        }
        if (!found) diffs++;
    }
    return diffs;
}

// GET /api/scans — list all scans (chunked response, low memory)
static esp_err_t api_scans_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    uint16_t head, count;
    esp_err_t err = scan_store_get_range(&head, &count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);

    uint8_t prev_bssids[CONFIG_LOCATOR_MAX_APS_PER_SCAN][6];
    uint8_t curr_bssids[CONFIG_LOCATOR_MAX_APS_PER_SCAN][6];
    uint8_t prev_count = 0;
    bool has_prev = false;
    bool first = true;
    char chunk[256];

    for (uint16_t i = head; i < count; i++) {
        stored_ap_t tmp_aps[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
        uint8_t ap_count = 0;
        err = scan_store_load(i, tmp_aps, CONFIG_LOCATOR_MAX_APS_PER_SCAN, &ap_count);
        if (err != ESP_OK) continue;

        // Extract BSSIDs for diff
        for (uint8_t j = 0; j < ap_count; j++) {
            memcpy(curr_bssids[j], tmp_aps[j].bssid, 6);
        }

        int diffs = -1;  // -1 = no previous scan
        if (has_prev) {
            diffs = bssid_diff(prev_bssids, prev_count, curr_bssids, ap_count);
        }

        int64_t timestamp = 0;
        scan_store_get_scan_info(i, NULL, &timestamp);

        int len = snprintf(chunk, sizeof(chunk),
                           "%s{\"id\":%u,\"aps\":%u,\"timestamp\":%lld",
                           first ? "" : ",", i, ap_count, (long long)timestamp);

        if (diffs >= 0) {
            len += snprintf(chunk + len, sizeof(chunk) - len, ",\"diffs\":%d", diffs);
        }

        // Include cached location if available
        scan_location_t loc;
        if (scan_store_get_location(i, &loc) == ESP_OK) {
            len += snprintf(chunk + len, sizeof(chunk) - len,
                            ",\"lat\":%.6f,\"lng\":%.6f,\"accuracy\":%.0f",
                            loc.lat, loc.lng, loc.accuracy);
        }

        len += snprintf(chunk + len, sizeof(chunk) - len, "}");
        httpd_resp_send_chunk(req, chunk, len);
        first = false;

        // Swap current to previous
        memcpy(prev_bssids, curr_bssids, ap_count * 6);
        prev_count = ap_count;
        has_prev = true;
    }

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// DELETE /api/scans — delete all scans
static esp_err_t api_scans_delete_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    esp_err_t err = scan_store_delete_all();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/scan?id=N — full scan detail
static esp_err_t api_scan_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char buf[16];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(buf, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_OK;
    }
    uint16_t id = (uint16_t)atoi(id_str);

    stored_ap_t aps[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
    uint8_t ap_count = 0;
    esp_err_t err = scan_store_load(id, aps, CONFIG_LOCATOR_MAX_APS_PER_SCAN, &ap_count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Scan not found");
        return ESP_OK;
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

    // Include cached location if available
    scan_location_t loc;
    if (scan_store_get_location(id, &loc) == ESP_OK) {
        cJSON *location = cJSON_AddObjectToObject(root, "location");
        cJSON_AddNumberToObject(location, "lat", loc.lat);
        cJSON_AddNumberToObject(location, "lng", loc.lng);
        cJSON_AddNumberToObject(location, "accuracy", loc.accuracy);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

// DELETE /api/scan?id=N — delete one scan
static esp_err_t api_scan_delete_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char buf[16];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(buf, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_OK;
    }
    uint16_t id = (uint16_t)atoi(id_str);

    esp_err_t err = scan_store_delete(id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Scan not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/locate?id=N — geolocate a scan (cached in NVS after first call)
static esp_err_t api_locate_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char buf[16];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(buf, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_OK;
    }
    uint16_t id = (uint16_t)atoi(id_str);

    double lat, lng, accuracy;
    bool cached = false;

    // Check NVS cache first
    scan_location_t loc;
    if (scan_store_get_location(id, &loc) == ESP_OK) {
        lat = loc.lat;
        lng = loc.lng;
        accuracy = loc.accuracy;
        cached = true;
        ESP_LOGI(TAG, "Location for scan %u served from cache", id);
    } else {
        // Load scan
        stored_ap_t aps[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
        uint8_t ap_count = 0;
        esp_err_t err = scan_store_load(id, aps, CONFIG_LOCATOR_MAX_APS_PER_SCAN, &ap_count);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Scan not found");
            return ESP_OK;
        }

        // Get API key
        char api_key[129];
        err = scan_store_get_api_key(api_key, sizeof(api_key));
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No API key configured");
            return ESP_OK;
        }

        // Call Google API
        geolocation_result_t result;
        err = geolocation_request(api_key, aps, ap_count, &result);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Geolocation failed");
            return ESP_OK;
        }

        lat = result.lat;
        lng = result.lng;
        accuracy = result.accuracy;

        // Cache in NVS
        scan_store_save_location(id, lat, lng, accuracy);
        ESP_LOGI(TAG, "Location for scan %u cached to NVS", id);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "lat", lat);
    cJSON_AddNumberToObject(resp, "lng", lng);
    cJSON_AddNumberToObject(resp, "accuracy", accuracy);
    cJSON_AddBoolToObject(resp, "cached", cached);

    // Include map embed URL so the API key is never sent to the frontend
    {
        char map_key[129] = {0};
        if (scan_store_get_api_key(map_key, sizeof(map_key)) == ESP_OK && map_key[0] != '\0') {
            char map_url[300];
            snprintf(map_url, sizeof(map_url),
                     "https://www.google.com/maps/embed/v1/place?key=%s&q=%.6f,%.6f&zoom=16",
                     map_key, lat, lng);
            cJSON_AddStringToObject(resp, "map_url", map_url);
        }
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

// GET /api/settings — get scan interval + whether API key is configured
static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char api_key[129] = {0};
    bool key_set = (scan_store_get_api_key(api_key, sizeof(api_key)) == ESP_OK && api_key[0] != '\0');

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "api_key_set", key_set);
    cJSON_AddNumberToObject(resp, "scan_interval", scan_store_get_scan_interval());

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

// POST /api/settings — save API key
static esp_err_t api_settings_post_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *key = cJSON_GetObjectItem(json, "api_key");
    if (key && cJSON_IsString(key) && key->valuestring[0] != '\0') {
        scan_store_set_api_key(key->valuestring);
    }

    cJSON *interval = cJSON_GetObjectItem(json, "scan_interval");
    if (interval && cJSON_IsNumber(interval)) {
        int val = interval->valueint;
        if (val >= 10 && val <= 3600) {
            scan_store_set_scan_interval((uint16_t)val);
        }
    }

    cJSON_Delete(json);
    esp_err_t err = ESP_OK;

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/sleep — enter deep sleep (start scanning)
static esp_err_t api_sleep_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Entering deep sleep...\"}");

    // Small delay so the response gets sent before we sleep
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_sleep_cb) {
        s_sleep_cb();
    }
    // Never reached if callback calls deep sleep
    return ESP_OK;
}

// GET /api/wifi/status — current WiFi mode, IP, SSID
static esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    cJSON *resp = cJSON_CreateObject();

    wifi_conn_mode_t mode = wifi_connect_get_mode();
    cJSON_AddStringToObject(resp, "mode", mode == WIFI_CONN_MODE_STA ? "STA" : "AP");

    char ip[16];
    wifi_connect_get_ip_str(ip, sizeof(ip));
    cJSON_AddStringToObject(resp, "ip", ip);

    char ssid[33] = {0};
    if (mode == WIFI_CONN_MODE_STA) {
        scan_store_get_wifi_ssid(ssid, sizeof(ssid));
    }
    cJSON_AddStringToObject(resp, "ssid", ssid);

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

// GET /api/wifi/scan — scan for nearby networks
static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char *json = wifi_connect_scan_networks();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

// POST /api/wifi/connect — save credentials and reboot
static esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    if (!ssid || !cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }

    char ssid_buf[33];
    char pass_buf[65];
    strncpy(ssid_buf, ssid->valuestring, sizeof(ssid_buf) - 1);
    ssid_buf[sizeof(ssid_buf) - 1] = '\0';
    strncpy(pass_buf, (pass && cJSON_IsString(pass)) ? pass->valuestring : "", sizeof(pass_buf) - 1);
    pass_buf[sizeof(pass_buf) - 1] = '\0';

    cJSON_Delete(json);

    // Send response before rebooting
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Connecting, device will reboot...\"}");

    // This saves creds and reboots
    wifi_connect_sta(ssid_buf, pass_buf);
    return ESP_OK;
}

// POST /api/wifi/forget — clear credentials and reboot to AP mode
static esp_err_t api_wifi_forget_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    scan_store_clear_wifi_creds();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Credentials cleared, rebooting to AP mode...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t uri_index = {
    .uri = "/", .method = HTTP_GET, .handler = index_get_handler
};
static const httpd_uri_t uri_favicon = {
    .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler
};
static const httpd_uri_t uri_scans_get = {
    .uri = "/api/scans", .method = HTTP_GET, .handler = api_scans_get_handler
};
static const httpd_uri_t uri_scans_delete = {
    .uri = "/api/scans", .method = HTTP_DELETE, .handler = api_scans_delete_handler
};
static const httpd_uri_t uri_scan_get = {
    .uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_get_handler
};
static const httpd_uri_t uri_scan_delete = {
    .uri = "/api/scan", .method = HTTP_DELETE, .handler = api_scan_delete_handler
};
static const httpd_uri_t uri_locate = {
    .uri = "/api/locate", .method = HTTP_POST, .handler = api_locate_handler
};
static const httpd_uri_t uri_settings_get = {
    .uri = "/api/settings", .method = HTTP_GET, .handler = api_settings_get_handler
};
static const httpd_uri_t uri_settings_post = {
    .uri = "/api/settings", .method = HTTP_POST, .handler = api_settings_post_handler
};
static const httpd_uri_t uri_sleep = {
    .uri = "/api/sleep", .method = HTTP_POST, .handler = api_sleep_handler
};
static const httpd_uri_t uri_wifi_status = {
    .uri = "/api/wifi/status", .method = HTTP_GET, .handler = api_wifi_status_handler
};
static const httpd_uri_t uri_wifi_scan = {
    .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler
};
static const httpd_uri_t uri_wifi_connect = {
    .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler
};
static const httpd_uri_t uri_wifi_forget = {
    .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = api_wifi_forget_handler
};
static const httpd_uri_t uri_api_options = {
    .uri = "/api/*", .method = HTTP_OPTIONS, .handler = api_options_handler
};

httpd_handle_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 17;
    config.stack_size = 10240;  // TLS handshake for Google API needs extra stack
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server");
        return NULL;
    }

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_favicon);
    httpd_register_uri_handler(server, &uri_scans_get);
    httpd_register_uri_handler(server, &uri_scans_delete);
    httpd_register_uri_handler(server, &uri_scan_get);
    httpd_register_uri_handler(server, &uri_scan_delete);
    httpd_register_uri_handler(server, &uri_locate);
    httpd_register_uri_handler(server, &uri_settings_get);
    httpd_register_uri_handler(server, &uri_settings_post);
    httpd_register_uri_handler(server, &uri_sleep);
    httpd_register_uri_handler(server, &uri_wifi_status);
    httpd_register_uri_handler(server, &uri_wifi_scan);
    httpd_register_uri_handler(server, &uri_wifi_connect);
    httpd_register_uri_handler(server, &uri_wifi_forget);
    httpd_register_uri_handler(server, &uri_api_options);

    ESP_LOGI(TAG, "Web server started");
    return server;
}

void web_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "Web server stopped");
    }
}
