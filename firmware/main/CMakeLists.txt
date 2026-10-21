# Sources follow the config; requirements cannot (they are resolved before
# sdkconfig is read), so both network components are listed.
if(CONFIG_XY_QEMU)
    set(net "wifi_qemu.c")
else()
    set(net "wifi.c")
endif()

idf_component_register(SRCS "main.c" "settings.c" "storage.c" ${net} "display.c" "http.c" "stream.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES xyaoled imaging httputil nvs_flash spi_flash esp_netif esp_wifi esp_eth
                                     esp_http_server esp_timer esp_app_format
                       # test.html: the Go daemon's test page (reference/go-daemon/testpage.html)
                       # with a stream colour selector.
                       EMBED_FILES "pages/config.html" "pages/test.html")
