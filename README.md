# ESP32 Locator

A battery-friendly WiFi reconnaissance and geolocation system for ESP32. The device cycles between deep sleep and WiFi scanning, storing nearby access point data in non-volatile storage. When woken by the boot button (or on first power-up), it serves a web UI for browsing scan results, geolocating scans via Google's API, and viewing positions on an embedded map.

## How It Works

The device operates in two modes, selected automatically by the wakeup cause:

### Scan Mode (timer wakeup)

1. ESP32 wakes from deep sleep
2. Performs an active WiFi scan (no connection needed)
3. Stores discovered access points (BSSID, RSSI, channel, auth mode, SSID) to NVS
4. Returns to deep sleep for the configured interval (default 60s)

Scans with zero APs are discarded. When NVS storage reaches capacity, the oldest scan is evicted.

### Web Server Mode (button press or power-on)

1. Onboard LED lights up to indicate the mode
2. Connects to the configured WiFi network
3. Syncs time via SNTP (pool.ntp.org)
4. Serves a single-page web UI on port 80

From the web UI you can:

- **Browse scans** -- table with index, timestamp, AP count, and a DIFFS column showing how many MAC addresses changed between consecutive scans (highlighted red when more than half the APs differ)
- **View scan details** -- full AP list with SSID, BSSID, signal strength bar, channel, and auth mode
- **Geolocate** -- sends scan data to the Google Geolocation API, displays coordinates and accuracy, renders position on an embedded Google Map
- **Configure** -- set the Google API key (masked input) and scan interval (10--3600 seconds)
- **Start Scanning** -- triggers deep sleep to begin scan cycles; press the BOOT button to return to web server mode

## Hardware Requirements

- ESP32 development board (e.g. ESP32-DevKitC)
- USB cable for power and programming
- No additional wiring needed -- the BOOT button (GPIO0) and onboard LED (GPIO2) are standard on most devkits

## Build and Flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.5 or later.

```bash
# Configure WiFi credentials
idf.py menuconfig
# → Example Connection Configuration → WiFi SSID / Password

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

Press `Ctrl+]` to exit the serial monitor.

## Configuration

### WiFi Credentials

Set under `idf.py menuconfig` → **Example Connection Configuration**.

### Locator Settings (menuconfig)

| Option | Default | Range | Description |
|--------|---------|-------|-------------|
| `LOCATOR_SCAN_INTERVAL_SEC` | 30 | 10--3600 | Deep sleep interval between scans |
| `LOCATOR_MAX_STORED_SCANS` | 50 | 10--200 | Max scans in NVS (oldest evicted) |
| `LOCATOR_MAX_APS_PER_SCAN` | 20 | 5--30 | Max APs recorded per scan |
| `LOCATOR_BOOT_BUTTON_GPIO` | 0 | -- | GPIO for boot button (9 for C3/C6) |
| `LOCATOR_LED_GPIO` | 2 | -- | GPIO for onboard LED |

### Runtime Settings (Web UI)

The scan interval and Google API key can also be changed at runtime through the Config page. These are stored in NVS and persist across reboots.

## Google API Setup

Two Google APIs are used:

1. **Geolocation API** -- the ESP32 sends scanned AP data via HTTPS and receives lat/lng coordinates. Enable it in the [Google Cloud Console](https://console.cloud.google.com/apis/library/geolocation.googleapis.com).
2. **Maps Embed API** -- the browser renders the location on a Google Map iframe. Enable it in the [Google Cloud Console](https://console.cloud.google.com/apis/library/maps-embed-backend.googleapis.com).

Both use the same API key, entered in the web UI's Config page.

## NVS Storage

Uses a custom partition table with 128KB NVS. Scan data is stored as compact binary blobs in a ring buffer:

- Each scan: 3-byte header + N x 42-byte AP records
- A scan with 20 APs is ~847 bytes
- With 128KB NVS, approximately 100+ scans fit comfortably

Timestamps are not stored per scan. Instead, an SNTP time base is recorded when the web server starts, and individual scan timestamps are computed from the time base, scan index, and scan interval.

## REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Serve web UI |
| GET | `/favicon.ico` | Serve favicon |
| GET | `/api/scans` | List all scans (id, timestamp, AP count, diffs) |
| GET | `/api/scan?id=N` | Full scan detail with all AP data |
| POST | `/api/locate?id=N` | Geolocate scan via Google API |
| DELETE | `/api/scan?id=N` | Delete one scan |
| DELETE | `/api/scans` | Delete all scans |
| GET | `/api/settings` | Get API key and scan interval |
| POST | `/api/settings` | Save API key and scan interval |
| POST | `/api/sleep` | Enter deep sleep (start scanning) |

## Project Structure

```
main/
  main.c              App entry point, mode selection, deep sleep, SNTP sync
  wifi_scan.c/h       WiFi scanning (STA mode, no connection)
  scan_store.c/h      NVS storage: save/load/delete scans, settings, ring buffer
  web_server.c/h      HTTP server and all URI handlers
  geolocation.c/h     Google Geolocation API client (HTTPS + cJSON)
  Kconfig.projbuild   Menuconfig options
  CMakeLists.txt      Build config, embedded files
  pages/
    index.html        Single-page web UI (Matrix-themed)
    favicon.png       Browser tab icon
partitions.csv        Custom partition table (128KB NVS)
sdkconfig.defaults    Partition table, TLS cert bundle, WiFi scan sorting
```
