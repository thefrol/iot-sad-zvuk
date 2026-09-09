#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

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
// Цифровая громкость 0..255 (64 = 25%), меняется параметром ?vol=
#define DEFAULT_VOLUME  64

#define RAW_CHUNK       2048
#define PCM_BUF_SIZE    8192

// DMA-буфер I2S: 16 дескрипторов x 512 фреймов ~= 185 мс звука
#define I2S_DMA_DESCS   16
#define I2S_DMA_FRAMES  512

// Кольцевой буфер между приёмом по сети и декодером (~4 с на 64 кбит/с)
#define STREAM_BUF_SIZE (32 * 1024)
// Сколько данных набрать до старта декодирования
#define PREBUFFER_BYTES (8 * 1024)

#define JOB_EMBEDDED    1
#define JOB_STREAM      2

extern const uint8_t sound_mp3_start[] asm("_binary_sound_mp3_start");
extern const uint8_t sound_mp3_end[]   asm("_binary_sound_mp3_end");

static i2s_chan_handle_t s_tx_handle;
static EventGroupHandle_t s_wifi_events;
#define WIFI_GOT_IP_BIT BIT0

static uint8_t s_volume = DEFAULT_VOLUME;
static volatile bool s_stop_requested;

static SemaphoreHandle_t s_session_mutex; // одна сессия воспроизведения
static SemaphoreHandle_t s_done_sem;      // плеер закончил сессию
static QueueHandle_t s_job_queue;         // задания плееру (JOB_*)
static StreamBufferHandle_t s_stream_buf; // сырые данные из сети
static volatile bool s_stream_done;       // тело запроса принято целиком

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Большой DMA-буфер = подушка против сетевых подёргиваний
    chan_cfg.dma_desc_num = I2S_DMA_DESCS;
    chan_cfg.dma_frame_num = I2S_DMA_FRAMES;
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
static int16_t s_last_l, s_last_r; // последний фрейм — для плавного затухания

static void i2s_write_pcm(const uint8_t *pcm, uint32_t len, uint8_t channels)
{
    // моно растягиваем в стерео, поэтому буфер с запасом x2
    static int16_t scaled[PCM_BUF_SIZE / sizeof(int16_t) * 2];
    size_t samples = len / sizeof(int16_t);
    size_t frames = samples / channels;

    size_t out_n = 0;
    if (channels == 2) {
        for (size_t i = 0; i < samples; i++) {
            scaled[i] = (int16_t)((((const int16_t *)pcm)[i] * s_volume) >> 8);
        }
        out_n = samples;
    } else { // моно -> дублируем в оба канала
        for (size_t i = 0; i < frames; i++) {
            int16_t s = (int16_t)((((const int16_t *)pcm)[i] * s_volume) >> 8);
            scaled[i * 2] = s;
            scaled[i * 2 + 1] = s;
        }
        out_n = frames * 2;
    }

    size_t written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(s_tx_handle, scaled, out_n * sizeof(int16_t),
                                      &written, portMAX_DELAY));
    if (out_n >= 2) {
        s_last_l = scaled[out_n - 2];
        s_last_r = scaled[out_n - 1];
    }
}

// После конца воспроизведения заливаем DMA-буфер нулями, иначе I2S
// продолжает крутить последний фрагмент звука («пек-пек-пек»).
// Сначала короткий спад от последнего сэмпла к нулю — резкий перепад
// в тишину слышен как щелчок.
static void i2s_flush_silence(void)
{
    static int16_t ramp[512 * 2]; // ~12 мс затухания
    for (int i = 0; i < 512; i++) {
        ramp[i * 2]     = (int16_t)((int32_t)s_last_l * (512 - i) / 512);
        ramp[i * 2 + 1] = (int16_t)((int32_t)s_last_r * (512 - i) / 512);
    }
    size_t written = 0;
    i2s_channel_write(s_tx_handle, ramp, sizeof(ramp), &written, portMAX_DELAY);
    s_last_l = s_last_r = 0;

    static const uint8_t zeros[PCM_BUF_SIZE];
    size_t total = I2S_DMA_DESCS * I2S_DMA_FRAMES * 4; // весь DMA-буфер канала
    while (total) {
        size_t chunk = total > sizeof(zeros) ? sizeof(zeros) : total;
        i2s_channel_write(s_tx_handle, zeros, chunk, &written, portMAX_DELAY);
        total -= written;
    }
}

// --- Wi-Fi ---

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "Wi-Fi отключён (причина %d, ssid %.*s), переподключаюсь...",
                 e->reason, e->ssid_len, e->ssid);
        xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (strlen(CONFIG_IOT_WIFI_SSID) == 0) {
        // Нет кредешелов -> поднимаем свою точку доступа для настройки/проверки
        esp_netif_create_default_wifi_ap();
        wifi_config_t ap = {
            .ap = {
                .ssid = "iot-sad-zvuk",
                .channel = 1,
                .authmode = WIFI_AUTH_OPEN,
                .max_connection = 2,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGW(TAG, "Wi-Fi не настроен. Точка доступа \"iot-sad-zvuk\", IP 192.168.4.1");
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
        return;
    }

    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_IOT_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_IOT_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode =
        strlen(CONFIG_IOT_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Иначе модем засыпает между биконами и recv встаёт до ~300 мс — звук заикается
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

// --- Декодер (работает в player_task) ---

typedef struct {
    esp_audio_simple_dec_handle_t dec;
    bool info_logged;
} player_ctx_t;

static bool decoder_open(player_ctx_t *ctx)
{
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .use_frame_dec = false,
    };
    esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &ctx->dec);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Не удалось открыть декодер: %d", err);
        return false;
    }
    ctx->info_logged = false;
    return true;
}

// Скармливает кусок сырых данных декодеру, PCM уходит в I2S
static esp_audio_err_t decode_feed(player_ctx_t *ctx, const uint8_t *data,
                                   uint32_t len, bool eos)
{
    static uint8_t pcm_buf[PCM_BUF_SIZE];

    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)data,
        .len = len,
        .eos = eos,
    };
    // do/while: при eos с len=0 нужен один вызов, чтобы декодер выдал хвост
    do {
        esp_audio_simple_dec_out_t out = {
            .buffer = pcm_buf,
            .len = PCM_BUF_SIZE,
        };
        esp_audio_err_t err = esp_audio_simple_dec_process(ctx->dec, &raw, &out);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Ошибка декодирования: %d", err);
            return err;
        }
        if (out.decoded_size) {
            esp_audio_simple_dec_info_t info = {0};
            esp_audio_simple_dec_get_info(ctx->dec, &info);
            if (!ctx->info_logged) {
                ESP_LOGI(TAG, "MP3: %lu Гц, %d кан., %d бит",
                         (unsigned long)info.sample_rate, info.channel,
                         info.bits_per_sample);
                if (info.sample_rate != SAMPLE_RATE) {
                    ESP_LOGW(TAG, "Частота потока %lu != %d, темп будет отличаться",
                             (unsigned long)info.sample_rate, SAMPLE_RATE);
                }
                ctx->info_logged = true;
            }
            i2s_write_pcm(out.buffer, out.decoded_size,
                          info.channel ? info.channel : 2);
        }
        raw.len -= raw.consumed;
        raw.buffer += raw.consumed;
    } while (raw.len);
    return ESP_AUDIO_ERR_OK;
}

// Встроенный MP3 целиком
static void play_embedded(void)
{
    player_ctx_t ctx = {0};
    if (!decoder_open(&ctx)) {
        return;
    }
    const uint8_t *data = sound_mp3_start;
    size_t left = sound_mp3_end - sound_mp3_start;

    while (left > 0 && !s_stop_requested) {
        size_t chunk = left > RAW_CHUNK ? RAW_CHUNK : left;
        if (decode_feed(&ctx, data, chunk, (chunk == left)) != ESP_AUDIO_ERR_OK) {
            break;
        }
        data += chunk;
        left -= chunk;
    }
    esp_audio_simple_dec_close(ctx.dec);
}

// Поток из сетевого кольцевого буфера
static void play_stream(void)
{
    player_ctx_t ctx = {0};
    if (!decoder_open(&ctx)) {
        return;
    }

    // Подушка перед стартом: ждём PREBUFFER_BYTES или конец приёма (макс ~3 с)
    for (int i = 0; i < 60 && !s_stream_done && !s_stop_requested; i++) {
        if (xStreamBufferBytesAvailable(s_stream_buf) >= PREBUFFER_BYTES) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    static uint8_t raw_buf[RAW_CHUNK];
    while (!s_stop_requested) {
        size_t n = xStreamBufferReceive(s_stream_buf, raw_buf, RAW_CHUNK,
                                        pdMS_TO_TICKS(250));
        if (n == 0) {
            if (s_stream_done) {
                break;
            }
            ESP_LOGW(TAG, "Буфер пуст, ждём сеть...");
            continue;
        }
        if (decode_feed(&ctx, raw_buf, n, false) != ESP_AUDIO_ERR_OK) {
            break;
        }
    }
    if (!s_stop_requested) {
        decode_feed(&ctx, raw_buf, 0, true); // eos: декодер выдаёт последний кадр
    }
    esp_audio_simple_dec_close(ctx.dec);
}

static void player_task(void *arg)
{
    int job;
    while (1) {
        if (xQueueReceive(s_job_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job == JOB_EMBEDDED) {
            play_embedded();
        } else {
            play_stream();
        }
        i2s_flush_silence();
        xSemaphoreGive(s_done_sem);
    }
}

// --- HTTP API ---

static void apply_volume_query(httpd_req_t *req)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }
    char val[8];
    if (httpd_query_key_value(query, "vol", val, sizeof(val)) == ESP_OK) {
        int v = atoi(val);
        if (v >= 0 && v <= 255) {
            s_volume = (uint8_t)v;
            ESP_LOGI(TAG, "Громкость: %d", v);
        }
    }
}

static esp_err_t reply_busy(httpd_req_t *req)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "уже играю, сначала POST /stop\n");
    return ESP_OK;
}

// POST /play — тело запроса это MP3-поток: принимаем в кольцевой буфер,
// декодирует и играет player_task. Ответ уходит по окончании воспроизведения.
static esp_err_t play_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(s_session_mutex, 0) != pdTRUE) {
        return reply_busy(req);
    }
    s_stop_requested = false;
    s_stream_done = false;
    xStreamBufferReset(s_stream_buf);
    apply_volume_query(req);

    int job = JOB_STREAM;
    xQueueSend(s_job_queue, &job, 0);

    static uint8_t net_buf[RAW_CHUNK];
    while (!s_stop_requested) {
        int n = httpd_req_recv(req, (char *)net_buf, RAW_CHUNK);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // клиент медленно шлёт — ждём
            }
            break; // 0 = тело закончилось, <0 = ошибка сокета
        }
        size_t sent = 0;
        while (sent < (size_t)n && !s_stop_requested) {
            sent += xStreamBufferSend(s_stream_buf, net_buf + sent, n - sent,
                                      pdMS_TO_TICKS(200));
        }
    }
    s_stream_done = true;

    // ждём, пока плеер доиграет буфер
    xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(60000));
    xSemaphoreGive(s_session_mutex);

    httpd_resp_sendstr(req, s_stop_requested ? "остановлено\n" : "ok\n");
    return ESP_OK;
}

// GET /beep — встроенный звук один раз (проверка связи/железа)
static esp_err_t beep_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(s_session_mutex, 0) != pdTRUE) {
        return reply_busy(req);
    }
    s_stop_requested = false;
    apply_volume_query(req);

    int job = JOB_EMBEDDED;
    xQueueSend(s_job_queue, &job, 0);
    xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(30000));
    xSemaphoreGive(s_session_mutex);

    httpd_resp_sendstr(req, s_stop_requested ? "остановлено\n" : "ok\n");
    return ESP_OK;
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    s_stop_requested = true;
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req,
        "iot-sad-zvuk\n"
        "  POST /play   — проиграть MP3 из тела запроса: curl -X POST --data-binary @f.mp3 http://IP/play\n"
        "  GET  /beep   — проиграть встроенный звук\n"
        "  POST /stop   — остановить воспроизведение\n"
        "  ?vol=0..255  — громкость (параметр /play и /beep)\n");
    return ESP_OK;
}

static void http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6 * 1024; // хендлер только кладёт данные в буфер
    config.max_uri_handlers = 8;
    config.recv_wait_timeout = 15;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t root = { .uri = "/",    .method = HTTP_GET,  .handler = root_handler };
    const httpd_uri_t play = { .uri = "/play", .method = HTTP_POST, .handler = play_handler };
    const httpd_uri_t beep = { .uri = "/beep", .method = HTTP_GET,  .handler = beep_handler };
    const httpd_uri_t stop = { .uri = "/stop", .method = HTTP_POST, .handler = stop_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &play));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &beep));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &stop));
    ESP_LOGI(TAG, "HTTP-сервер запущен на порту %d", config.server_port);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_session_mutex = xSemaphoreCreateMutex();
    s_done_sem = xSemaphoreCreateBinary();
    s_job_queue = xQueueCreate(1, sizeof(int));
    s_stream_buf = xStreamBufferCreate(STREAM_BUF_SIZE, 1);
    s_wifi_events = xEventGroupCreate();

    i2s_init();
    ESP_ERROR_CHECK(esp_audio_dec_register_default());
    ESP_ERROR_CHECK(esp_audio_simple_dec_register_default());
    xTaskCreate(player_task, "player", 16 * 1024, NULL, 5, NULL);

    wifi_init();
    http_server_start();

    if (strlen(CONFIG_IOT_WIFI_SSID) > 0) {
        ESP_LOGI(TAG, "Подключаюсь к Wi-Fi \"%s\"...", CONFIG_IOT_WIFI_SSID);
    }
    xEventGroupWaitBits(s_wifi_events, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Готово. Пример: curl -X POST --data-binary @sound.mp3 http://<IP>/play");
}
