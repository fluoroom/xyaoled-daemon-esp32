idf_component_register(SRCS "main.c" "settings.c" "storage.c" "wifi.c" "display.c" "http.c" "stream.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES xyaoled imaging httputil nvs_flash spi_flash esp_wifi esp_netif
                                     esp_http_server esp_timer esp_app_format
                       EMBED_FILES "pages/config.html"
                                   # The Go daemon's test page, used as is (git submodule).
                                   "../../reference/go-daemon/testpage.html")
