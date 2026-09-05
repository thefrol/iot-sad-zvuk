idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES esp_audio_codec esp_driver_i2s esp_driver_gpio log freertos esp_timer
                    EMBED_FILES "sound.mp3")
