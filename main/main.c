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

static const char *TAG = "locator";

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

    // Need netif and event loop for WiFi scan
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(scan_store_init());

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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(scan_store_init());

    // Connect to WiFi (STA with stored creds, or SoftAP fallback)
    wifi_conn_mode_t mode = wifi_connect_init();

    if (mode == WIFI_CONN_MODE_STA) {
        // SNTP time sync (only meaningful in STA mode with internet)
        sntp_sync();
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
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Determine operating mode from wakeup cause
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

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
            ESP_LOGI(TAG, "Wakeup: POWER ON / RESET -> web server mode");
            run_web_server_mode();
            break;
    }
}
