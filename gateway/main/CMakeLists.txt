idf_component_register(SRCS "main.c" "gw_wifi.c" "gw_link.c" "gw_set_policy.c" "gw_console.c" "gw_keys.c" "gw_prov.c" "gw_audio.c" "gw_sd.c" "gw_wake.c" "gw_voice_tools.c" "gw_voice_ws.c" "tts_resample.c" "voice_vad.c" "gw_ota_policy.c"
                            "gw_http.c" "gw_ota.c" "gw_node_ota.c" "gw_api.c" "gw_cloud.c" "gw_display.c"
                            "ui/ui.c" "ui/ui_pages.c" "ui/ui_nodes.c" "ui/ui_overlay.c"
                            "ui/fonts/font_cn_16.c" "ui/fonts/font_cn_24.c"
                        INCLUDE_DIRS "." "ui"
                       EMBED_FILES "prov.html"
                       REQUIRES meshproto esp_wifi esp_timer app_update esp_netif nvs_flash console
                                esp_http_server cjson esp_adc esp_driver_spi esp_driver_i2c esp_lcd mqtt esp-tls
                                  esp_http_client mbedtls esp_driver_i2s esp_codec_dev esp_audio_codec esp_websocket_client fatfs sdmmc esp-sr)
