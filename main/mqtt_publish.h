#pragma once

#include "esp_err.h"

// Publish latest scan via MQTT. Also publishes all scans if cycle threshold reached.
// Call when WiFi is connected (e.g., from open_wifi hook).
esp_err_t mqtt_publish_scans(void);
