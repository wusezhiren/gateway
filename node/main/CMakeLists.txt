idf_component_register(SRCS "main.c" "node_io.c" "node_link.c" "node_ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES meshproto esp_wifi esp_timer app_update esp_netif nvs_flash esp_driver_gpio esp_driver_ledc esp_driver_i2c mbedtls)
