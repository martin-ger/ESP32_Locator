# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32 Locator: a dual-mode WiFi scanner and geolocation application built on ESP-IDF 5.5.2. The project name in the build system is `esp32_locator`.

**Two operating modes:**
- **Scan Mode** (default on timer wakeup, or power-on if `boot_mode=1`): wake from deep sleep, WiFi scan, store to NVS, optionally connect to home WiFi or open WiFi for MQTT publish + SNTP sync, deep sleep
- **Web Server Mode** (button press, or power-on if `boot_mode=0` [default]): connect WiFi, SNTP sync, serve web UI for browsing scans and geolocating via Google API

## Build Commands

Requires ESP-IDF installed with `IDF_PATH` environment variable set.

```bash
idf.py build                          # Build the project
idf.py -p /dev/ttyUSB0 flash          # Flash to board
idf.py -p /dev/ttyUSB0 monitor        # Serial monitor (Ctrl-] to exit)
idf.py -p /dev/ttyUSB0 flash monitor  # Combined flash + monitor
idf.py menuconfig                     # Configure WiFi, scan settings
```

Note: When changing `sdkconfig.defaults`, delete `sdkconfig` and run `idf.py fullclean` before building.

## Architecture

**File structure:**
- `main/main.c` — app_main: NVS init, wakeup cause detection, mode branching, deep sleep config
- `main/wifi_scan.h/c` — WiFi STA init (no connect), blocking scan, convert to stored format, deinit
- `main/scan_store.h/c` — NVS operations: save/load/list/delete scans, settings, MQTT config, blocklist
- `main/web_server.h/c` — HTTP server lifecycle, all URI handlers
- `main/geolocation.h/c` — Build JSON, HTTPS POST to Google Geolocation API, parse response with cJSON
- `main/open_wifi.h/c` — Opportunistic open WiFi connection, captive portal handling, SNTP sync
- `main/mqtt_publish.h/c` — MQTT client: publish scans as retained JSON to configured broker/topic
- `main/wifi_connect.h/c` — WiFi connection management (STA + SoftAP fallback)
- `main/pages/index.html` — SPA embedded via CMake EMBED_TXTFILES
- `partitions.csv` — Custom partition table with 512KB NVS
- `locator.html` — Standalone local analyzer (browser-only, imports JSON or connects to ESP)
- `mqtt_sub.sh` — Shell script to subscribe to MQTT topic and save JSON files for locator.html

**Web Server Endpoints:**
- `GET /` — Serve index.html SPA
- `GET /api/scans` — JSON array of scan summaries
- `GET /api/scan?id=N` — Full scan detail as JSON
- `POST /api/locate?id=N` — Call Google Geolocation API, return lat/lng/accuracy
- `DELETE /api/scan?id=N` — Delete one scan
- `DELETE /api/scans` — Delete all scans
- `GET /api/settings` — Get all settings (API key, MQTT config, scan interval, etc.)
- `POST /api/settings` — Save settings (JSON body)
- `POST /api/sleep` — Enter deep sleep (start scanning), resets MQTT cycle counter
- `GET /api/wifi/status` — Current WiFi mode, IP, SSID
- `GET /api/wifi/scan` — Scan for nearby WiFi networks
- `POST /api/wifi/connect` — Save WiFi credentials and reboot
- `POST /api/wifi/forget` — Clear WiFi credentials and reboot to AP mode
- `GET /api/blocklist` — List blocklisted open WiFi SSIDs
- `DELETE /api/blocklist` — Clear entire blocklist
- `DELETE /api/blocklist?ssid=X` — Delete single blocklist entry

**NVS Storage** (namespace `locator`):
- `scan_count` (u16): monotonic counter
- `scan_head` (u16): oldest scan still stored (ring buffer)
- `sNNNNN` (blob): packed scan data (header + AP records)
- `lNNNNN` (blob): cached location per scan (lat, lng, accuracy)
- `api_key` (string): Google API key
- `scan_ivl` (u16): scan interval in seconds
- `web_pass` (string): web server password (HTTP Basic Auth)
- `wifi_ssid` / `wifi_pass` (string): stored WiFi credentials
- `boot_mode` (u8): default boot mode (0=web server, 1=scan mode)
- `ow_mode` (u8): open WiFi mode (0=off, 1=sync, 2=MQTT+sync)
- `mqtt_url_l` / `mqtt_url_a` (string): MQTT URLs for last/all scans
- `mqtt_wait` (u16): wait cycles between "publish all" operations
- `mqtt_cid` / `mqtt_user` / `mqtt_pass` (string): MQTT credentials
- `mqtt_cycle` (u16): current cycle counter for "publish all"
- `bl0`..`bl9` (string): open WiFi SSID blocklist (FIFO ring buffer)

**MQTT Publishing** (scan mode, open WiFi mode 2):
- URL format: `mqtt://broker:port/topic/path` — path portion is the MQTT topic
- Last scan: published as retained QoS 0 JSON to the "last" topic every cycle
- All scans: published as retained QoS 0 JSON array to the "all" topic every N cycles
- Cycle counter resets when entering scan mode from the web UI
- JSON format matches `/api/scan` output (id, timestamp, aps with auth, location if cached)

## Configuration

Under `idf.py menuconfig` > "Locator Configuration" (defined in `main/Kconfig.projbuild`):
- `LOCATOR_SCAN_INTERVAL_SEC` — Deep sleep interval (default 30s)
- `LOCATOR_MAX_STORED_SCANS` — Max scans in NVS (default 500)
- `LOCATOR_MAX_APS_PER_SCAN` — Max APs per scan (default 10)
- `LOCATOR_BOOT_BUTTON_GPIO` — Boot button GPIO (default 0, 9 for C3/C6)
- `LOCATOR_LED_GPIO` — Onboard LED GPIO (default 2, 8 for C3/C6)
- `LOCATOR_OPEN_WIFI_ENABLED` — Enable opportunistic open WiFi connection (default y)

WiFi SSID/password and MQTT settings are configured at runtime through the web UI.

## Troubleshooting

If `httpd_parse: parse_block: request URI/header too long` appears, increase `HTTPD_MAX_REQ_HDR_LEN` in menuconfig: Component config > HTTP Server > Max HTTP Request Header Length.
