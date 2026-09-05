#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"

static const char *TAG = "zvuk";

// --- Распиновка MAX98357A (ESP32-C3) ---
// BCLK -> GPIO1, LRCK (WS) -> GPIO3, DIN -> GPIO4
// VIN усилителя -> 5V, GND -> GND, SD в воздухе (включён), GAIN -> VDD
#define I2S_BCLK_GPIO   GPIO_NUM_1
#define I2S_WS_GPIO     GPIO_NUM_3
#define I2S_DOUT_GPIO   GPIO_NUM_4

#define SAMPLE_RATE     44100
// Цифровая громкость 0..255 (64 = 25%)
#define VOLUME          64

#define RAW_CHUNK       2048
#define PCM_BUF_SIZE    8192

extern const uint8_t sound_mp3_start[] asm("_binary_sound_mp3_start");
extern const uint8_t sound_mp3_end[]   asm("_binary_sound_mp3_end");

static i2s_chan_handle_t s_tx_handle;

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_LOGI(TAG, "I2S запущен: %d Гц, BCLK=%d WS=%d DOUT=%d",
             SAMPLE_RATE, I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO);
}

// Масштабирование громкости + отправка PCM в I2S
static void i2s_write_pcm(const uint8_t *pcm, uint32_t len, uint8_t channels)
{
    static int16_t scaled[PCM_BUF_SIZE / sizeof(int16_t)]; // моно растягиваем в стерео
    size_t samples = len / sizeof(int16_t);
    size_t frames = samples / channels;

    size_t out_n = 0;
    if (channels == 2) {
        for (size_t i = 0; i < samples; i++) {
            scaled[i] = (int16_t)((((const int16_t *)pcm)[i] * VOLUME) >> 8);
        }
        out_n = samples;
    } else { // моно -> дублируем в оба канала
        for (size_t i = 0; i < frames; i++) {
            int16_t s = (int16_t)((((const int16_t *)pcm)[i] * VOLUME) >> 8);
            scaled[i * 2] = s;
            scaled[i * 2 + 1] = s;
        }
        out_n = frames * 2;
    }

    size_t written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(s_tx_handle, scaled, out_n * sizeof(int16_t),
                                      &written, portMAX_DELAY));
}

// Проигрывает встроенный MP3 целиком, затем возвращается
static void play_mp3(void)
{
    static uint8_t pcm_buf[PCM_BUF_SIZE];
    static bool info_logged = false;

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &dec);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Не удалось открыть декодер: %d", err);
        return;
    }

    const uint8_t *data = sound_mp3_start;
    size_t left = sound_mp3_end - sound_mp3_start;

    while (left > 0) {
        size_t chunk = left > RAW_CHUNK ? RAW_CHUNK : left;
        esp_audio_simple_dec_raw_t raw = {
            .buffer = (uint8_t *)data,
            .len = (uint32_t)chunk,
            .eos = (left == chunk),
        };
        data += chunk;
        left -= chunk;

        while (raw.len) {
            esp_audio_simple_dec_out_t out = {
                .buffer = pcm_buf,
                .len = PCM_BUF_SIZE,
            };
            err = esp_audio_simple_dec_process(dec, &raw, &out);
            if (err != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Ошибка декодирования: %d", err);
                goto done;
            }
            if (out.decoded_size) {
                if (!info_logged) {
                    esp_audio_simple_dec_info_t info = {0};
                    esp_audio_simple_dec_get_info(dec, &info);
                    ESP_LOGI(TAG, "MP3: %lu Гц, %d кан., %d бит",
                             (unsigned long)info.sample_rate, info.channel,
                             info.bits_per_sample);
                    if (info.sample_rate != SAMPLE_RATE) {
                        ESP_LOGW(TAG, "Частота файла %lu != %d, темп будет отличаться",
                                 (unsigned long)info.sample_rate, SAMPLE_RATE);
                    }
                    info_logged = true;
                }
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(dec, &info);
                i2s_write_pcm(out.buffer, out.decoded_size,
                              info.channel ? info.channel : 2);
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
    }

done:
    esp_audio_simple_dec_close(dec);
}

// Воспроизведение в отдельной задаче — декодеру нужен большой стек
static void player_task(void *arg)
{
    ESP_LOGI(TAG, "Играем sound.mp3 в цикле, пауза 5 с");
    while (1) {
        play_mp3();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    i2s_init();
    ESP_ERROR_CHECK(esp_audio_dec_register_default());
    ESP_ERROR_CHECK(esp_audio_simple_dec_register_default());
    xTaskCreate(player_task, "player", 16 * 1024, NULL, 5, NULL);
}
