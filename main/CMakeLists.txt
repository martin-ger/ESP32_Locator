set(srcs "main.c" "wifi_scan.c" "scan_store.c" "web_server.c" "geolocation.c" "wifi_connect.c")

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_netif esp_http_server esp_wifi
                                  esp_http_client esp-tls json driver esp_timer
                    EMBED_TXTFILES "pages/index.html"
                    EMBED_FILES "pages/favicon.png")
