#include "open_wifi.h"
#include "scan_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <ctype.h>
#include <time.h>

static const char *TAG = "open_wifi";

// --- State ---
static open_wifi_hook_t s_hook = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;
static volatile bool s_got_ip = false;
static volatile bool s_connected = false;
static esp_netif_t *s_netif = NULL;
static esp_event_handler_instance_t s_wifi_handler_inst = NULL;
static esp_event_handler_instance_t s_ip_handler_inst = NULL;

// --- Connectivity check result ---
typedef enum {
    CONN_DIRECT,   // 204 — real internet
    CONN_PORTAL,   // 3xx — captive portal redirect
    CONN_FAIL,     // No connectivity
} conn_status_t;

// --- Form field for captive portal ---
#define MAX_FORM_FIELDS 16
#define MAX_FIELD_NAME  64
#define MAX_FIELD_VALUE 128

typedef struct {
    char name[MAX_FIELD_NAME];
    char value[MAX_FIELD_VALUE];
} form_field_t;

// --- Portal body buffer size ---
#define PORTAL_BODY_SIZE 8192
#define FORM_POST_SIZE   2048
#define URL_BUF_SIZE     512

void open_wifi_set_hook(open_wifi_hook_t hook)
{
    s_hook = hook;
}

// ========== WiFi lifecycle ==========

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "WiFi disconnected");
            s_connected = false;
            s_got_ip = false;
            xSemaphoreGive(s_connect_sem);
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            s_connected = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_got_ip = true;
        xSemaphoreGive(s_connect_sem);
    }
}

static esp_err_t wifi_init(void)
{
    s_connect_sem = xSemaphoreCreateBinary();
    if (!s_connect_sem) return ESP_ERR_NO_MEM;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        vSemaphoreDelete(s_connect_sem);
        s_connect_sem = NULL;
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        vSemaphoreDelete(s_connect_sem);
        s_connect_sem = NULL;
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_handler_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_handler_inst));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static void wifi_deinit_full(void)
{
    if (s_wifi_handler_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_inst);
        s_wifi_handler_inst = NULL;
    }
    if (s_ip_handler_inst) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_inst);
        s_ip_handler_inst = NULL;
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    if (s_connect_sem) {
        vSemaphoreDelete(s_connect_sem);
        s_connect_sem = NULL;
    }

    s_got_ip = false;
    s_connected = false;
}

// ========== String helpers ==========

static const char *strcasestr_bounded(const char *haystack, const char *needle, size_t max_len)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return haystack;
    if (max_len < needle_len) return NULL;

    for (size_t i = 0; i <= max_len - needle_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return &haystack[i];
    }
    return NULL;
}

// Extract a quoted attribute value from an HTML tag.
// tag_start points to '<', tag_end points past '>'.
static bool extract_attr(const char *tag_start, const char *tag_end,
                         const char *attr_name, char *out, size_t out_size)
{
    size_t tag_len = tag_end - tag_start;
    const char *p = strcasestr_bounded(tag_start, attr_name, tag_len);
    if (!p) return false;

    p += strlen(attr_name);
    // Skip whitespace and '='
    while (p < tag_end && (*p == ' ' || *p == '\t')) p++;
    if (p >= tag_end || *p != '=') return false;
    p++;
    while (p < tag_end && (*p == ' ' || *p == '\t')) p++;
    if (p >= tag_end) return false;

    char quote = 0;
    if (*p == '"' || *p == '\'') {
        quote = *p;
        p++;
    }

    const char *start = p;
    if (quote) {
        while (p < tag_end && *p != quote) p++;
    } else {
        while (p < tag_end && *p != ' ' && *p != '\t' && *p != '>') p++;
    }

    size_t len = p - start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t si = 0, di = 0;
    while (src[si] && di < dst_size - 1) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            if (di + 3 > dst_size - 1) break;
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
        si++;
    }
    dst[di] = '\0';
    return di;
}

static void resolve_url(const char *base_url, const char *relative, char *out, size_t out_size)
{
    if (!relative || !relative[0]) {
        strncpy(out, base_url, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    // Already absolute
    if (strncmp(relative, "http://", 7) == 0 || strncmp(relative, "https://", 8) == 0) {
        strncpy(out, relative, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    // Protocol-relative
    if (relative[0] == '/' && relative[1] == '/') {
        snprintf(out, out_size, "http:%s", relative);
        return;
    }

    // Extract scheme + host from base_url
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) {
        strncpy(out, relative, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');

    if (relative[0] == '/') {
        // Absolute path
        size_t host_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);
        if (host_len >= out_size) host_len = out_size - 1;
        memcpy(out, base_url, host_len);
        out[host_len] = '\0';
        size_t remaining = out_size - host_len;
        strncat(out, relative, remaining - 1);
    } else {
        // Relative path — append to base directory
        if (path_start) {
            const char *last_slash = strrchr(path_start, '/');
            size_t dir_len = last_slash ? (size_t)(last_slash - base_url + 1) : strlen(base_url);
            if (dir_len >= out_size) dir_len = out_size - 1;
            memcpy(out, base_url, dir_len);
            out[dir_len] = '\0';
            size_t remaining = out_size - dir_len;
            strncat(out, relative, remaining - 1);
        } else {
            snprintf(out, out_size, "%s/%s", base_url, relative);
        }
    }
}

// ========== WiFi connect to specific SSID ==========

static bool confirm_and_connect(const char *ssid)
{
    // Quick re-scan to confirm SSID is still present with decent signal
    wifi_scan_config_t scan_config = {
        .ssid = (uint8_t *)ssid,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 150,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Re-scan failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGW(TAG, "'%s' not found in re-scan", ssid);
        return false;
    }

    wifi_ap_record_t *records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!records) return false;
    esp_wifi_scan_get_ap_records(&ap_num, records);

    bool found = false;
    for (uint16_t i = 0; i < ap_num; i++) {
        if (strcmp((char *)records[i].ssid, ssid) == 0 && records[i].rssi > -80) {
            found = true;
            break;
        }
    }
    free(records);

    if (!found) {
        ESP_LOGW(TAG, "'%s' too weak or gone", ssid);
        return false;
    }

    // Attempt connection (up to 2 tries)
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    for (int attempt = 0; attempt < 2; attempt++) {
        s_got_ip = false;
        s_connected = false;

        ESP_LOGI(TAG, "Connecting to '%s' (attempt %d)", ssid, attempt + 1);
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            continue;
        }

        if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(15000)) == pdTRUE && s_got_ip) {
            ESP_LOGI(TAG, "Connected to '%s'", ssid);
            return true;
        }

        ESP_LOGW(TAG, "Connection attempt %d timed out", attempt + 1);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return false;
}

// ========== HTTP helpers ==========

typedef struct {
    char *buf;
    int len;
    int capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (!resp) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len < resp->capacity) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

// ========== Connectivity check ==========

static conn_status_t check_connectivity(char *redirect_url, size_t redirect_url_size)
{
    if (!s_connected) return CONN_FAIL;

    http_response_t resp = {0};

    esp_http_client_config_t config = {
        .url = "http://connectivitycheck.gstatic.com/generate_204",
        .disable_auto_redirect = true,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return CONN_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connectivity check failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return CONN_FAIL;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Connectivity check: HTTP %d", status);

    if (status == 204) {
        esp_http_client_cleanup(client);
        return CONN_DIRECT;
    }

    if (status >= 300 && status < 400) {
        // Extract Location header for redirect
        if (redirect_url && redirect_url_size > 0) {
            // Re-read the header - use esp_http_client_get_header
            // The Location header should be available
            char *location = NULL;
            esp_http_client_get_header(client, "Location", &location);
            if (location) {
                strncpy(redirect_url, location, redirect_url_size - 1);
                redirect_url[redirect_url_size - 1] = '\0';
                ESP_LOGI(TAG, "Portal redirect: %s", redirect_url);
            } else {
                redirect_url[0] = '\0';
            }
        }
        esp_http_client_cleanup(client);
        return CONN_PORTAL;
    }

    // Some portals return 200 with HTML instead of redirect
    if (status == 200) {
        esp_http_client_cleanup(client);
        // Not a 204 means portal or blocked
        if (redirect_url) redirect_url[0] = '\0';
        return CONN_PORTAL;
    }

    esp_http_client_cleanup(client);
    return CONN_FAIL;
}

// ========== Follow redirects and get portal page ==========

static esp_err_t fetch_portal_page(const char *url, char *body, int body_size, char *final_url, size_t final_url_size)
{
    char current_url[URL_BUF_SIZE];
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';

    for (int hop = 0; hop < 5; hop++) {
        if (!s_connected) return ESP_FAIL;

        http_response_t resp = {
            .buf = body,
            .len = 0,
            .capacity = body_size,
        };
        body[0] = '\0';

        esp_http_client_config_t config = {
            .url = current_url,
            .disable_auto_redirect = true,
            .timeout_ms = 10000,
            .event_handler = http_event_handler,
            .user_data = &resp,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return ESP_FAIL;

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int status = esp_http_client_get_status_code(client);

        if (status >= 300 && status < 400) {
            char *location = NULL;
            esp_http_client_get_header(client, "Location", &location);
            if (location && location[0]) {
                char next_url[URL_BUF_SIZE];
                resolve_url(current_url, location, next_url, sizeof(next_url));
                strncpy(current_url, next_url, sizeof(current_url) - 1);
                current_url[sizeof(current_url) - 1] = '\0';
                ESP_LOGI(TAG, "Redirect hop %d -> %s", hop + 1, current_url);
                esp_http_client_cleanup(client);
                continue;
            }
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        esp_http_client_cleanup(client);

        if (status == 200) {
            if (final_url) {
                strncpy(final_url, current_url, final_url_size - 1);
                final_url[final_url_size - 1] = '\0';
            }
            ESP_LOGI(TAG, "Portal page fetched (%d bytes)", resp.len);
            return ESP_OK;
        }

        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Too many redirects");
    return ESP_FAIL;
}

// ========== Captive portal form handler ==========

static esp_err_t handle_captive_portal(const char *body, const char *base_url)
{
    size_t body_len = strlen(body);

    // Find <form
    const char *form_start = strcasestr_bounded(body, "<form", body_len);
    if (!form_start) {
        ESP_LOGW(TAG, "No <form> found in portal page");
        return ESP_FAIL;
    }

    // Find end of <form> tag
    const char *form_tag_end = strchr(form_start, '>');
    if (!form_tag_end) return ESP_FAIL;
    form_tag_end++;

    // Find </form>
    const char *form_close = strcasestr_bounded(form_tag_end, "</form", body_len - (form_tag_end - body));
    if (!form_close) {
        form_close = body + body_len;  // Use rest of document
    }

    // Extract action attribute
    char action[URL_BUF_SIZE] = "";
    extract_attr(form_start, form_tag_end, "action", action, sizeof(action));

    // Resolve action URL
    char action_url[URL_BUF_SIZE];
    if (action[0]) {
        resolve_url(base_url, action, action_url, sizeof(action_url));
    } else {
        strncpy(action_url, base_url, sizeof(action_url) - 1);
        action_url[sizeof(action_url) - 1] = '\0';
    }

    // Extract method (default POST)
    char method[8] = "POST";
    extract_attr(form_start, form_tag_end, "method", method, sizeof(method));

    // Parse <input> fields
    form_field_t fields[MAX_FORM_FIELDS];
    int field_count = 0;

    const char *p = form_tag_end;
    while (p < form_close && field_count < MAX_FORM_FIELDS) {
        const char *input = strcasestr_bounded(p, "<input", form_close - p);
        if (!input) break;

        const char *input_end = strchr(input, '>');
        if (!input_end) break;
        input_end++;

        char type[32] = "";
        char name[MAX_FIELD_NAME] = "";
        char value[MAX_FIELD_VALUE] = "";

        extract_attr(input, input_end, "type", type, sizeof(type));
        extract_attr(input, input_end, "name", name, sizeof(name));
        extract_attr(input, input_end, "value", value, sizeof(value));

        // Convert type to lowercase
        for (char *t = type; *t; t++) *t = tolower((unsigned char)*t);

        if (strcmp(type, "password") == 0) {
            ESP_LOGW(TAG, "Portal requires password — blocklisting");
            return ESP_FAIL;
        }

        if (name[0] == '\0') {
            p = input_end;
            continue;
        }

        if (strcmp(type, "hidden") == 0 || strcmp(type, "submit") == 0) {
            strncpy(fields[field_count].name, name, MAX_FIELD_NAME - 1);
            strncpy(fields[field_count].value, value, MAX_FIELD_VALUE - 1);
            field_count++;
        } else if (strcmp(type, "text") == 0 || strcmp(type, "email") == 0) {
            strncpy(fields[field_count].name, name, MAX_FIELD_NAME - 1);
            strncpy(fields[field_count].value, "anon-66@yahoo.com", MAX_FIELD_VALUE - 1);
            field_count++;
        } else if (strcmp(type, "checkbox") == 0) {
            strncpy(fields[field_count].name, name, MAX_FIELD_NAME - 1);
            strncpy(fields[field_count].value, "on", MAX_FIELD_VALUE - 1);
            field_count++;
        }

        p = input_end;
    }

    if (field_count == 0) {
        ESP_LOGW(TAG, "No form fields found");
        return ESP_FAIL;
    }

    // Build URL-encoded POST body
    char *post_body = malloc(FORM_POST_SIZE);
    if (!post_body) return ESP_ERR_NO_MEM;

    int pos = 0;
    for (int i = 0; i < field_count && pos < FORM_POST_SIZE - 1; i++) {
        if (i > 0 && pos < FORM_POST_SIZE - 1) post_body[pos++] = '&';

        char encoded_name[MAX_FIELD_NAME * 3];
        char encoded_value[MAX_FIELD_VALUE * 3];
        url_encode(fields[i].name, encoded_name, sizeof(encoded_name));
        url_encode(fields[i].value, encoded_value, sizeof(encoded_value));

        int written = snprintf(post_body + pos, FORM_POST_SIZE - pos,
                               "%s=%s", encoded_name, encoded_value);
        if (written > 0) pos += written;
    }
    post_body[pos] = '\0';

    ESP_LOGI(TAG, "Submitting portal form to %s (%d fields)", action_url, field_count);

    // POST the form
    esp_http_client_config_t config = {
        .url = action_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    esp_err_t err = esp_http_client_open(client, pos);
    if (err != ESP_OK) {
        free(post_body);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_write(client, post_body, pos);
    free(post_body);

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Portal form submit: HTTP %d", status);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return ESP_OK;
}

// ========== SNTP sync ==========

static void do_sntp_sync(void)
{
    ESP_LOGI(TAG, "Starting SNTP sync...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    int retry = 0;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 10) {
        ESP_LOGI(TAG, "SNTP waiting... (%d/10)", retry);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (retry >= 10) {
        ESP_LOGW(TAG, "SNTP sync timed out");
    } else {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "SNTP synced: %s", buf);
    }

    esp_sntp_stop();
}

// ========== Main entry point ==========

esp_err_t open_wifi_try(const char **ssids, uint8_t ssid_count)
{
    if (ssid_count == 0) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "Trying %u open WiFi SSIDs", ssid_count);

    esp_err_t init_err = wifi_init();
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(init_err));
        return ESP_FAIL;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;

    for (uint8_t i = 0; i < ssid_count; i++) {
        const char *ssid = ssids[i];
        ESP_LOGI(TAG, "--- Trying SSID '%s' (%u/%u) ---", ssid, i + 1, ssid_count);

        // Step 1: Blocklist check
        if (scan_store_blocklist_contains(ssid)) {
            ESP_LOGI(TAG, "'%s' is blocklisted, skipping", ssid);
            continue;
        }

        // Step 2: Confirm and connect
        if (!confirm_and_connect(ssid)) {
            ESP_LOGW(TAG, "Failed to connect to '%s'", ssid);
            continue;
        }

        // Step 3: Check connectivity
        char redirect_url[URL_BUF_SIZE] = "";
        conn_status_t conn = check_connectivity(redirect_url, sizeof(redirect_url));

        if (conn == CONN_FAIL) {
            ESP_LOGW(TAG, "'%s' — no connectivity", ssid);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (conn == CONN_PORTAL) {
            ESP_LOGI(TAG, "'%s' — captive portal detected", ssid);

            // Step 5: Fetch portal page
            char *portal_body = malloc(PORTAL_BODY_SIZE);
            if (!portal_body) {
                ESP_LOGE(TAG, "No memory for portal body");
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            char final_url[URL_BUF_SIZE] = "";
            esp_err_t fetch_err;

            if (redirect_url[0]) {
                fetch_err = fetch_portal_page(redirect_url, portal_body, PORTAL_BODY_SIZE,
                                              final_url, sizeof(final_url));
            } else {
                // Some portals return 200 directly on connectivity check.
                // Try fetching any page to get the portal.
                fetch_err = fetch_portal_page("http://connectivitycheck.gstatic.com/generate_204",
                                              portal_body, PORTAL_BODY_SIZE,
                                              final_url, sizeof(final_url));
            }

            if (fetch_err != ESP_OK || !s_connected) {
                ESP_LOGW(TAG, "Failed to fetch portal page");
                free(portal_body);
                scan_store_blocklist_add(ssid);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // Step 6: Handle captive portal form
            const char *base = final_url[0] ? final_url : redirect_url;
            esp_err_t portal_err = handle_captive_portal(portal_body, base);
            free(portal_body);

            if (portal_err != ESP_OK || !s_connected) {
                ESP_LOGW(TAG, "Portal handling failed, blocklisting '%s'", ssid);
                scan_store_blocklist_add(ssid);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // Step 7: Re-check connectivity after portal submission
            vTaskDelay(pdMS_TO_TICKS(2000));  // Give portal time to activate
            char dummy[URL_BUF_SIZE];
            conn_status_t recheck = check_connectivity(dummy, sizeof(dummy));
            if (recheck != CONN_DIRECT) {
                ESP_LOGW(TAG, "Still captive after form submit, blocklisting '%s'", ssid);
                scan_store_blocklist_add(ssid);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        // Step 8: User hook (request before sync)
        ESP_LOGI(TAG, "'%s' — internet access confirmed!", ssid);
        if (s_hook) {
            esp_err_t hook_err = s_hook();
            if (hook_err != ESP_OK) {
                ESP_LOGW(TAG, "Hook returned error: %s (non-fatal)", esp_err_to_name(hook_err));
            }
        }

        // Step 9: SNTP sync
        do_sntp_sync();

        // Step 10: Success
        result = ESP_OK;
        break;
    }

    // Always deinit WiFi before returning
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_deinit_full();

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Open WiFi session completed successfully");
    } else {
        ESP_LOGI(TAG, "No open WiFi candidate worked");
    }

    return result;
}

// ========== Home WiFi (with password) ==========

esp_err_t open_wifi_try_home(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Trying home WiFi '%s'", ssid);

    esp_err_t err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;

    // Configure with SSID and password
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    // Attempt connection (up to 2 tries)
    for (int attempt = 0; attempt < 2; attempt++) {
        s_got_ip = false;
        s_connected = false;

        ESP_LOGI(TAG, "Connecting to '%s' (attempt %d)", ssid, attempt + 1);
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            continue;
        }

        if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(15000)) == pdTRUE && s_got_ip) {
            ESP_LOGI(TAG, "Connected to home WiFi '%s'", ssid);
            break;
        }

        ESP_LOGW(TAG, "Connection attempt %d timed out", attempt + 1);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!s_got_ip) {
        ESP_LOGW(TAG, "Failed to connect to home WiFi '%s'", ssid);
        goto cleanup;
    }

    // Verify connectivity (skip captive portal handling)
    {
        char dummy[URL_BUF_SIZE];
        conn_status_t conn = check_connectivity(dummy, sizeof(dummy));
        if (conn != CONN_DIRECT) {
            ESP_LOGW(TAG, "Home WiFi '%s' — no internet connectivity", ssid);
            goto cleanup;
        }
    }

    ESP_LOGI(TAG, "Home WiFi '%s' — internet access confirmed!", ssid);

    // User hook (e.g. MQTT publish)
    if (s_hook) {
        esp_err_t hook_err = s_hook();
        if (hook_err != ESP_OK) {
            ESP_LOGW(TAG, "Hook returned error: %s (non-fatal)", esp_err_to_name(hook_err));
        }
    }

    // SNTP sync
    do_sntp_sync();

    result = ESP_OK;

cleanup:
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_deinit_full();

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Home WiFi session completed successfully");
    }
    return result;
}
