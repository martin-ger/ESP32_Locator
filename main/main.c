#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"

#include "wifi_scan.h"
#include "scan_store.h"
#include "web_server.h"
#include "wifi_connect.h"
#include <mdns.h>
#ifdef CONFIG_LOCATOR_OPEN_WIFI_ENABLED
#include "open_wifi.h"
#include "mqtt_publish.h"
#endif

static const char *TAG = "locator";

#ifdef CONFIG_LOCATOR_OPEN_WIFI_ENABLED
static esp_err_t mqtt_publish_hook(void)
{
    ESP_LOGI(TAG, "Open WiFi hook: MQTT publish");
    esp_err_t err = mqtt_publish_scans();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT publish failed: %s (non-fatal)", esp_err_to_name(err));
    }
    return ESP_OK;
}
#endif

static void start_mdns_service() {
    esp_err_t err = mdns_init();
    if (err) {
        ESP_LOGW(TAG, "Error in starting mDNS!");
        return;
    }

    mdns_hostname_set("locator");
    mdns_instance_name_set("ESP32 Locator");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

static void enter_deep_sleep(void)
{
    // NVS must be initialized before calling this
    // (scan_store_init is called in both modes before we get here)
    uint16_t interval = scan_store_get_scan_interval();

    ESP_LOGI(TAG, "Configuring deep sleep: timer=%us, button=GPIO%d",
             interval, CONFIG_LOCATOR_BOOT_BUTTON_GPIO);

    esp_sleep_enable_timer_wakeup((uint64_t)interval * 1000000ULL);
    esp_sleep_enable_ext0_wakeup(CONFIG_LOCATOR_BOOT_BUTTON_GPIO, 0);
    rtc_gpio_pullup_en(CONFIG_LOCATOR_BOOT_BUTTON_GPIO);
    rtc_gpio_pulldown_dis(CONFIG_LOCATOR_BOOT_BUTTON_GPIO);

    ESP_LOGI(TAG, "Entering deep sleep...");
    esp_deep_sleep_start();
}

static void run_scan_mode(void)
{
    ESP_LOGI(TAG, "=== SCAN MODE ===");

    stored_ap_t aps[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
    uint16_t ap_count = wifi_scan_execute(aps, CONFIG_LOCATOR_MAX_APS_PER_SCAN);

    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found, skipping storage");
        enter_deep_sleep();
        return;  // Never reached
    }

    time_t now;
    time(&now);
    ESP_LOGI(TAG, "Scanned %u APs, saving to NVS (epoch=%lld)", ap_count, (long long)now);
    uint16_t index;
    esp_err_t err = scan_store_save(aps, (uint8_t)ap_count, (int64_t)now, &index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save scan: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved scan #%u", index);
    }

#ifdef CONFIG_LOCATOR_OPEN_WIFI_ENABLED
    uint8_t ow_mode = scan_store_get_open_wifi_mode();
    if (ow_mode != OPEN_WIFI_OFF) {
        // Set hook based on mode: sync-only → no hook, MQTT+sync → publish hook
        if (ow_mode == OPEN_WIFI_REQ) {
            open_wifi_set_hook(mqtt_publish_hook);
        } else {
            open_wifi_set_hook(NULL);
        }

        // Light LED during WiFi attempt
        gpio_config_t led_conf = {
            .pin_bit_mask = (1ULL << CONFIG_LOCATOR_LED_GPIO),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&led_conf);
        gpio_set_level(CONFIG_LOCATOR_LED_GPIO, 1);

        bool wifi_done = false;

        // Try home WiFi first if credentials are configured and SSID is in scan results
        char home_ssid[33] = {0};
        char home_pass[65] = {0};
        if (scan_store_has_wifi_creds() &&
            scan_store_get_wifi_ssid(home_ssid, sizeof(home_ssid)) == ESP_OK &&
            scan_store_get_wifi_pass(home_pass, sizeof(home_pass)) == ESP_OK) {

            // Check if home SSID appears in scan results
            for (uint16_t j = 0; j < ap_count; j++) {
                size_t len = aps[j].ssid_len;
                if (len > 32) len = 32;
                char scanned_ssid[33];
                memcpy(scanned_ssid, aps[j].ssid, len);
                scanned_ssid[len] = '\0';
                if (strcmp(scanned_ssid, home_ssid) == 0) {
                    ESP_LOGI(TAG, "Home WiFi '%s' found in scan results, attempting connection", home_ssid);
                    if (open_wifi_try_home(home_ssid, home_pass) == ESP_OK) {
                        wifi_done = true;
                    }
                    break;
                }
            }
        }

        // Fall through to open WiFi if home WiFi didn't work
        if (!wifi_done) {
            // Extract unique open SSIDs from scan results
            const char *open_ssids[CONFIG_LOCATOR_MAX_APS_PER_SCAN];
            char ssid_bufs[CONFIG_LOCATOR_MAX_APS_PER_SCAN][33];
            uint8_t open_count = 0;

            for (uint16_t j = 0; j < ap_count; j++) {
                if (aps[j].authmode != 0 || aps[j].ssid_len == 0) continue;

                // Null-terminate SSID
                size_t len = aps[j].ssid_len;
                if (len > 32) len = 32;
                memcpy(ssid_bufs[open_count], aps[j].ssid, len);
                ssid_bufs[open_count][len] = '\0';

                // Deduplicate
                bool dup = false;
                for (uint8_t k = 0; k < open_count; k++) {
                    if (strcmp(open_ssids[k], ssid_bufs[open_count]) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                open_ssids[open_count] = ssid_bufs[open_count];
                open_count++;
            }

            if (open_count > 0) {
                ESP_LOGI(TAG, "Found %u open WiFi network(s), mode=%u, attempting connection",
                         open_count, ow_mode);
                open_wifi_try(open_ssids, open_count);
            }
        }

        gpio_set_level(CONFIG_LOCATOR_LED_GPIO, 0);
    }
#endif

    enter_deep_sleep();
}

static void sntp_sync(void)
{
    ESP_LOGI(TAG, "Starting SNTP sync...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait for time to be set (max 15 seconds)
    int retry = 0;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 30) {
        ESP_LOGI(TAG, "Waiting for SNTP... (%d)", retry);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (retry >= 30) {
        ESP_LOGW(TAG, "SNTP sync timed out");
    } else {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "SNTP synced: %s (epoch=%lld)", buf, (long long)now);
    }

    esp_sntp_stop();
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server) {
        web_server_stop(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL) {
        *server = web_server_start();
    }
}

static void run_web_server_mode(void)
{
    ESP_LOGI(TAG, "=== WEB SERVER MODE ===");

    // Connect to WiFi (STA with stored creds, or SoftAP fallback)
    wifi_conn_mode_t mode = wifi_connect_init();

    if (mode == WIFI_CONN_MODE_STA) {
        // SNTP time sync (only meaningful in STA mode with internet)
        sntp_sync();
        // mDNS to reach the ESP at "locator.local"
        start_mdns_service();
    } else {
        ESP_LOGI(TAG, "AP mode — connect to 'ESP32_Locator' WiFi, open http://192.168.4.1");
    }

    // Start web server (works in both STA and AP modes)
    web_server_set_sleep_callback(enter_deep_sleep);
    static httpd_handle_t server = NULL;
    server = web_server_start();

    // Light up onboard LED only after web server has started
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << CONFIG_LOCATOR_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_conf);
    gpio_set_level(CONFIG_LOCATOR_LED_GPIO, 1);

    if (mode == WIFI_CONN_MODE_STA) {
        // Register reconnect handlers (STA only)
        ESP_ERROR_CHECK(esp_event_handler_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
        ESP_ERROR_CHECK(esp_event_handler_register(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    }

    char ip_buf[16];
    wifi_connect_get_ip_str(ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "Web server running at http://%s — Press Ctrl+] to exit monitor.", ip_buf);

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Determine operating mode from wakeup cause
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

    // Need NVS namespace open before checking boot_mode
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(scan_store_init());

    switch (wakeup) {
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wakeup: TIMER -> scan mode");
            run_scan_mode();
            break;

        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI(TAG, "Wakeup: BUTTON -> web server mode");
            run_web_server_mode();
            break;

        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            if (scan_store_get_boot_mode() == BOOT_MODE_SCAN) {
                ESP_LOGI(TAG, "Wakeup: POWER ON / RESET -> scan mode (configured)");
                run_scan_mode();
            } else {
                ESP_LOGI(TAG, "Wakeup: POWER ON / RESET -> web server mode");
                run_web_server_mode();
            }
            break;
    }
}
