# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32 Locator: a dual-mode WiFi scanner and geolocation application built on ESP-IDF 5.5.2. The project name in the build system is `esp32_locator`.

**Two operating modes:**
- **Scan Mode** (default on timer wakeup): wake from deep sleep, WiFi scan, store to NVS, deep sleep 30s
- **Web Server Mode** (button press or power-on): connect WiFi, SNTP sync, serve web UI for browsing scans and geolocating via Google API

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
- `main/scan_store.h/c` — NVS operations: save/load/list/delete scans, API key get/set, ring-buffer eviction
- `main/web_server.h/c` — HTTP server lifecycle, all URI handlers
- `main/geolocation.h/c` — Build JSON, HTTPS POST to Google Geolocation API, parse response with cJSON
- `main/pages/index.html` — SPA embedded via CMake EMBED_TXTFILES
- `partitions.csv` — Custom partition table with 64KB NVS

**Web Server Endpoints:**
- `GET /` — Serve index.html SPA
- `GET /api/scans` — JSON array of scan summaries
- `GET /api/scan?id=N` — Full scan detail as JSON
- `POST /api/locate?id=N` — Call Google Geolocation API, return lat/lng/accuracy
- `DELETE /api/scan?id=N` — Delete one scan
- `DELETE /api/scans` — Delete all scans
- `GET /api/settings` — Get API key
- `POST /api/settings` — Save API key (JSON body)

**NVS Storage** (namespace `locator`):
- `scan_count` (u16): monotonic counter
- `scan_head` (u16): oldest scan still stored (ring buffer)
- `sNNNNN` (blob): packed scan data (header + AP records)
- `api_key` (string): Google API key
- `time_base` (i64) + `time_base_idx` (u16): for timestamp computation

## Configuration

Under `idf.py menuconfig` > "Locator Configuration" (defined in `main/Kconfig.projbuild`):
- `LOCATOR_SCAN_INTERVAL_SEC` — Deep sleep interval (default 30s)
- `LOCATOR_MAX_STORED_SCANS` — Max scans in NVS (default 50)
- `LOCATOR_MAX_APS_PER_SCAN` — Max APs per scan (default 20)
- `LOCATOR_BOOT_BUTTON_GPIO` — Boot button GPIO (default 0)

WiFi SSID/password are configured under "Example Connection Configuration".

## Troubleshooting

If `httpd_parse: parse_block: request URI/header too long` appears, increase `HTTPD_MAX_REQ_HDR_LEN` in menuconfig: Component config > HTTP Server > Max HTTP Request Header Length.
