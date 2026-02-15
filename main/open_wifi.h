#pragma once

#include "esp_err.h"
#include <stdint.h>

// Callback invoked after successful open WiFi connection + SNTP sync
typedef esp_err_t (*open_wifi_hook_t)(void);

// Set the callback to invoke when connected to an open WiFi network.
void open_wifi_set_hook(open_wifi_hook_t hook);

// Try connecting to open WiFi networks from the given SSID list (sorted by preference).
// Returns ESP_OK if connected+used+disconnected successfully.
// Returns ESP_ERR_NOT_FOUND if no candidate worked.
// WiFi is fully deinitialized on return regardless of outcome.
esp_err_t open_wifi_try(const char **ssids, uint8_t ssid_count);

// Try connecting to the home WiFi (with password) for SNTP/hook.
// WiFi is fully deinitialized on return regardless of outcome.
esp_err_t open_wifi_try_home(const char *ssid, const char *password);
