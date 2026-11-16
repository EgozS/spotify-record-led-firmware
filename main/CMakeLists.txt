idf_component_register(SRCS "main.cpp" "net.cpp" "disc.cpp" "config.cpp" "portal.cpp" "screen.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_http_server
                             esp-tls mbedtls lwip)
