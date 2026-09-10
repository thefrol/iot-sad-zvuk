idf_component_register(SRCS "main.c" "ota_update.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES esp_audio_codec esp_driver_i2s esp_driver_gpio log freertos esp_timer
                                  esp_wifi esp_event esp_netif nvs_flash esp_http_server
                                  mqtt esp_http_client esp_https_ota app_update esp_app_format espressif__cjson
                    EMBED_FILES "sound.mp3")
