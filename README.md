# ESP32 Locator

A battery-friendly WiFi reconnaissance and geolocation system for ESP32. The tool uses only WiFi, no GPS required. In densly populated areas with any kind of WiFi cells this works with an accuracy of 10-20 meters. It uses WiFi scans
and the Google GeoLocation API. Moderate use of this API should be free, but you will need your own API key (see below).

The device cycles between deep sleep and WiFi scanning, storing nearby access point data in non-volatile storage. When woken by the boot button (or on first power-up), it serves a web UI for browsing scan results, geolocating scans via Google's API, and viewing positions on an embedded map.

If you don't have an API key or if you don't want to deploy it on an embedded device, you can simply use the ESP32 Locator as scanner and export the scans for later use.

## How It Works

The device operates in two modes, selected automatically by the wakeup cause:

### Scan Mode (timer wakeup)

1. ESP32 wakes from deep sleep
2. Performs an active WiFi scan (no connection needed)
3. Stores discovered access points (BSSID, RSSI, channel, auth mode, SSID) to NVS
4. Returns to deep sleep for the configured interval (default 60s)

Scans with zero APs are discarded. When NVS storage reaches capacity, the oldest scan is evicted.

### Web Server Mode (button press or power-on)

1. Attempts to connect to a stored WiFi network (STA mode)
2. If no credentials are stored or connection fails after 5 retries, starts a SoftAP captive portal ("ESP32_Locator", open, 192.168.4.1)
3. Syncs time via SNTP in STA mode (pool.ntp.org)
4. Starts the web server on port 80
5. Onboard LED lights up to indicate the web server is ready

From the web UI you can:

- **Browse scans** -- table with index, timestamp, AP count, DIFFS column (highlighted red when more than half the APs differ), cached location indicator, and distance to previous located scan
- **View scan details** -- full AP list with SSID, BSSID, signal strength bar, channel, and auth mode
- **Geolocate** -- sends scan data to the Google Geolocation API, displays coordinates and accuracy, renders position on an embedded Google Map. Results are cached in NVS so repeat views are instant without another API call
- **Export** -- downloads all scan data (including cached locations) as a JSON file named `LocatorScan_<date>_<time>.json`
- **Configure WiFi** -- scan for nearby networks, select and enter credentials; the device reboots into STA mode. "Forget" clears stored credentials and reboots into AP mode
- **Configure settings** -- set the Google API key (masked input) and scan interval (10--3600 seconds)
- **Start Scanning** -- triggers deep sleep to begin scan cycles; press the BOOT button to return to web server mode

### First Boot (or after "Forget")

1. The device starts a SoftAP named **ESP32_Locator** (open, no password)
2. Connect your phone or laptop to this network
3. Open **http://192.168.4.1** in a browser
4. Go to the **Config** page
5. Click **Scan** to list nearby WiFi networks
6. Select your network, enter the password, and click **Connect**
7. The device reboots and connects in STA mode with a DHCP address

### Changing Networks

Open the Config page, enter new credentials or use Scan to find networks, and click Connect. The device reboots to the new network.

### Resetting WiFi

Click **Forget** on the Config page to clear stored credentials. The device reboots into AP mode so you can reconfigure.

### Fallback

If the stored network is unavailable (wrong password, out of range), the device falls back to AP mode after 5 retries (~30 seconds), so you can always reconfigure via the captive portal.

## On-Device Analysis

The scan data collected by the ESP32 enables several types of analysis directly from the web UI, without any external tools:

### Movement Detection

The **DIFFS** column in the scan overview shows the symmetric difference between consecutive scans -- how many MAC addresses appeared or disappeared. A low diff (0--3) means the device is stationary. A high diff (highlighted red when >50% of APs change) indicates the device has moved to a different location. This works without any API calls or internet connection.

### Location Tracking

By geolocating individual scans via the Google Geolocation API, you can determine where the device was at each scan timestamp. Located scans are marked with a bold "Locate" button in the overview. The location is cached in NVS after the first API call, so viewing the same location again is instant.

### Distance and Path

Once multiple scans are geolocated, the overview automatically computes the **Haversine distance** between consecutively located scans. The "dist" column shows the straight-line distance in meters or kilometers, forming a path of the device's movement over time.

### Signal Environment Fingerprinting

Each scan captures a snapshot of the RF environment: which networks are visible, their signal strengths, channels, and security modes. By comparing scans you can:

- Identify recurring locations (same set of BSSIDs at similar RSSI levels)
- Detect new or disappeared access points in an area
- Observe signal strength variations indicating movement within a building

### Data Export

The Export function downloads all scan data as JSON, including full AP details and any cached location coordinates. This data can be imported into external tools for further analysis, mapping, or archival.

## Hardware Requirements

- ESP32 development board (e.g. ESP32-DevKitC)
- 4MB flash (standard on most devkits)
- USB cable for power and programming
- No additional wiring needed -- the BOOT button (GPIO0) and onboard LED (GPIO2) are standard on most devkits

## Build and Flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.5 or later.

```bash
# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

Press `Ctrl+]` to exit the serial monitor.

No compile-time WiFi configuration is needed -- credentials are set at runtime through the web UI.

## Configuration

### Locator Settings (menuconfig)

| Option | Default | Range | Description |
|--------|---------|-------|-------------|
| `LOCATOR_SCAN_INTERVAL_SEC` | 30 | 10--3600 | Deep sleep interval between scans |
| `LOCATOR_MAX_STORED_SCANS` | 500 | 10--1000 | Max scans in NVS (oldest evicted) |
| `LOCATOR_MAX_APS_PER_SCAN` | 10 | 5--30 | Max APs recorded per scan |
| `LOCATOR_BOOT_BUTTON_GPIO` | 0 | -- | GPIO for boot button (9 for C3/C6) |
| `LOCATOR_LED_GPIO` | 2 | -- | GPIO for onboard LED |

### Runtime Settings (Web UI)

The scan interval, Google API key, and WiFi credentials can be changed at runtime through the Config page. All settings are stored in NVS and persist across reboots.

## Google API Setup

Two Google APIs are used:

1. **Geolocation API** -- the ESP32 sends scanned AP data via HTTPS and receives lat/lng coordinates. Enable it in the [Google Cloud Console](https://console.cloud.google.com/apis/library/geolocation.googleapis.com).
2. **Maps Embed API** -- the browser renders the location on a Google Map iframe. Enable it in the [Google Cloud Console](https://console.cloud.google.com/apis/library/maps-embed-backend.googleapis.com).

Both use the same API key, entered in the web UI's Config page.

## NVS Storage

Uses a custom partition table with 512KB NVS on 4MB flash. Data stored in NVS:

- **Scan data** -- compact binary blobs in a ring buffer (11-byte header + N x 42-byte AP records). A scan with 10 APs is ~431 bytes.
- **Location cache** -- 24-byte blob per geolocated scan (lat, lng, accuracy as doubles). Cached on first API call, served directly on subsequent requests.
- **WiFi credentials** -- SSID and password strings.
- **Settings** -- API key and scan interval.

With 512KB NVS, approximately 500 scans with locations fit comfortably.

## REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Serve web UI |
| GET | `/favicon.ico` | Serve favicon |
| GET | `/api/scans` | List all scans (id, timestamp, AP count, diffs, location if cached) |
| GET | `/api/scan?id=N` | Full scan detail with all AP data and location |
| POST | `/api/locate?id=N` | Geolocate scan (cached after first call) |
| DELETE | `/api/scan?id=N` | Delete one scan |
| DELETE | `/api/scans` | Delete all scans |
| GET | `/api/settings` | Get scan interval and whether API key is set |
| POST | `/api/settings` | Save API key and scan interval |
| POST | `/api/sleep` | Enter deep sleep (start scanning) |
| GET | `/api/wifi/status` | Current WiFi mode, IP, SSID |
| GET | `/api/wifi/scan` | Scan for nearby WiFi networks |
| POST | `/api/wifi/connect` | Save WiFi credentials and reboot |
| POST | `/api/wifi/forget` | Clear WiFi credentials and reboot to AP mode |

## Project Structure

```
main/
  main.c              App entry point, mode selection, deep sleep, SNTP sync
  wifi_scan.c/h       WiFi scanning (STA mode, no connection)
  wifi_connect.c/h    WiFi connection management (STA + SoftAP fallback)
  scan_store.c/h      NVS storage: scans, locations, settings, WiFi credentials
  web_server.c/h      HTTP server and all URI handlers (CORS enabled)
  geolocation.c/h     Google Geolocation API client (HTTPS + cJSON)
  Kconfig.projbuild   Menuconfig options
  CMakeLists.txt      Build config, embedded files
  pages/
    index.html        Single-page web UI (Matrix-themed)
    favicon.png       Browser tab icon
locator.html          Standalone local analyzer (see below)
partitions.csv        Custom partition table (512KB NVS, 4MB flash)
sdkconfig.defaults    Flash size, partition table, TLS cert bundle, WiFi scan sorting
```

## Local Analyzer (`locator.html`)

A standalone HTML/JS application that runs entirely in a local browser, independent of the ESP32's embedded web UI. Open `locator.html` directly in a browser (double-click or `file://` URL).

### Data Sources

- **Connect to ESP** -- enter the ESP32's IP address to fetch all scans over the local network (requires the ESP to be in web server mode). CORS headers on the ESP allow cross-origin access.
- **Import JSON** -- load a previously exported scan file. Location data included in the export is restored automatically, and already-located scans are marked in the overview.

### Features

- Same terminal-style interface as the on-device UI
- **Google API key stored locally** in the browser's `localStorage` -- never sent to the ESP
- **Client-side geolocation** -- calls the Google Geolocation API directly from the browser, no proxy needed
- **Locate All** -- batch-geolocate all unlocated scans with a progress bar
- **Export** -- downloads all scan data including location results as JSON (re-importable)
- Scan overview with AP count, diffs, distances between consecutive located scans
