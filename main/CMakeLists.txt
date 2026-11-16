idf_component_register(SRCS "main.cpp" "net.cpp" "disc.cpp" "config.cpp" "portal.cpp" "screen.cpp" "counter.cpp" "ota.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_http_server
                             esp-tls mbedtls lwip app_update esp_app_format)
