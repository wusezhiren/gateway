idf_component_register(SRCS "main.c" "gw_wifi.c" "gw_link.c" "gw_console.c"
                       INCLUDE_DIRS "."
                       REQUIRES meshproto esp_wifi esp_timer app_update esp_netif nvs_flash console)
