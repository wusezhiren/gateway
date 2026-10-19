idf_component_register(SRCS "main.c" "gw_wifi.c" "gw_link.c" "gw_console.c" "gw_keys.c" "gw_prov.c"
                            "gw_http.c" "gw_ota.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "prov.html"
                       REQUIRES meshproto esp_wifi esp_timer app_update esp_netif nvs_flash console
                                esp_http_server json esp_adc)
