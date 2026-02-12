#pragma once

#include "wifi_scan.h"
#include "esp_err.h"

typedef struct {
    double lat;
    double lng;
    double accuracy;
} geolocation_result_t;

// Call Google Geolocation API with the given APs.
// api_key: Google API key string
// aps: array of AP records
// ap_count: number of APs
// result: output location
esp_err_t geolocation_request(const char *api_key, const stored_ap_t *aps,
                              uint8_t ap_count, geolocation_result_t *result);
