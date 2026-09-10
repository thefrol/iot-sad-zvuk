#include "ota_update.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

// Потокобезопасное состояние OTA, доступное для HTTP/MQTT-интерфейсов.
static SemaphoreHandle_t s_state_mux = NULL;
static ota_status_t s_status = {0};

// Буфер для тела ответа GitHub API. JSON релиза с парой ассетов — ~6 КБ
// (проверено на ebbflow-lamp), берём с двойным запасом. Больше нельзя:
// буфер живёт одновременно с TLS-буферами (~50 КБ), а свободной кучи
// после загрузки всего ~65 КБ; переполнение обрывает запрос.
#define API_BUF_CAP (16 * 1024)

typedef struct {
    char *buf;
    size_t len;
} http_body_t;

static esp_err_t http_on_event(esp_http_client_event_t *evt)
{
    http_body_t *body = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        // Буфер выделяется лениво: одновременно с TLS-рукопожатием
        // (+32 КБ к ~50 КБ mbedTLS) его malloc просто не давал памяти —
        // рукопожатие падало в верификации сертификата с PSA -141.
        if (!body->buf) {
            body->buf = malloc(API_BUF_CAP);
            if (!body->buf) {
                ESP_LOGE(TAG, "нет памяти под буфер ответа API");
                return ESP_FAIL;
            }
        }
        if (body->len + evt->data_len >= API_BUF_CAP) {
            ESP_LOGE(TAG, "ответ API не влез в %d байт", API_BUF_CAP);
            return ESP_FAIL;
        }
        memcpy(body->buf + body->len, evt->data, evt->data_len);
        body->len += evt->data_len;
    }
    return ESP_OK;
}

// Скачивает URL целиком в память, возвращает буфер (освобождает вызывающий)
// или NULL при ошибке.
static char *http_get(const char *url)
{
    http_body_t body = {.buf = NULL, .len = 0};

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_on_event,
        .user_data = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    // GitHub API отвечает 403 на запросы без User-Agent
    esp_http_client_set_header(client, "User-Agent", "iot-sad-zvuk-ota");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || !body.buf) {
        ESP_LOGW(TAG, "GET %s: err=%s, HTTP %d", url, esp_err_to_name(err), status);
        free(body.buf);
        return NULL;
    }
    body.buf[body.len] = '\0';
    return body.buf;
}

// "1.2.3" или "v1.2.3" -> {1,2,3}. false, если не похоже на semver.
static bool parse_version(const char *s, int v[3])
{
    if (s[0] == 'v') {
        s++;
    }
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

// Тег релиза новее текущей прошивки?
static bool release_is_newer(const char *tag)
{
    const char *t = (tag[0] == 'v') ? tag + 1 : tag;
    const char *cur = esp_app_get_description()->version;
    if (strcmp(cur, t) == 0) {
        return false;
    }
    int c[3], r[3];
    if (!parse_version(cur, c)) {
        // Своя версия не semver (локальная сборка) — считаем релиз новее
        return true;
    }
    if (!parse_version(t, r)) {
        ESP_LOGW(TAG, "тег релиза '%s' не похож на версию, игнорирую", tag);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (r[i] != c[i]) {
            return r[i] > c[i];
        }
    }
    return false;
}

// Ищет в JSON релиза ассет под текущий чип, возвращает его download URL
// (строка внутри json — живёт, пока жив json) или NULL.
static const char *find_asset_url(cJSON *json, char *out_tag, size_t tag_size)
{
    cJSON *tag = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
    if (!cJSON_IsString(tag)) {
        return NULL;
    }
    snprintf(out_tag, tag_size, "%s", tag->valuestring);

    char asset_name[64];
    snprintf(asset_name, sizeof(asset_name), "iot-sad-zvuk-%s.bin", CONFIG_IDF_TARGET);

    cJSON *assets = cJSON_GetObjectItemCaseSensitive(json, "assets");
    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        if (cJSON_IsString(name) && strcmp(name->valuestring, asset_name) == 0) {
            cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            return cJSON_IsString(url) ? url->valuestring : NULL;
        }
    }
    ESP_LOGW(TAG, "в релизе нет ассета %s", asset_name);
    return NULL;
}

static esp_err_t ota_from_url(const char *url)
{
    ESP_LOGI(TAG, "скачиваю обновление: %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        // После 302-редиректа GitHub отдаёт подписанный URL длиной ~900+
        // байт — в дефолтный буфер 512 он не влезает и запрос обрывается.
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_REBOOT_PENDING;
        xSemaphoreGive(s_state_mux);
        ESP_LOGI(TAG, "обновление записано, перезагрузка");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA не удалось: %s", esp_err_to_name(err));
    return err;
}

static void ota_check_once(bool auto_download)
{
    char api_url[128];
    snprintf(api_url, sizeof(api_url),
             "https://api.github.com/repos/%s/releases/latest", CONFIG_ZVUK_OTA_REPO);

    ESP_LOGI(TAG, "heap: free %u, min %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));

    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    s_status.state = OTA_STATE_CHECKING;
    s_status.error_message[0] = '\0';
    xSemaphoreGive(s_state_mux);

    char *body = http_get(api_url);
    if (!body) {
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "не удалось получить ответ от GitHub API");
        xSemaphoreGive(s_state_mux);
        return;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "не удалось разобрать JSON ответа API");
        xSemaphoreGive(s_state_mux);
        return;
    }

    char tag[32] = {0};
    const char *bin_url = find_asset_url(json, tag, sizeof(tag));
    const char *cur = esp_app_get_description()->version;

    if (bin_url && release_is_newer(tag)) {
        ESP_LOGI(TAG, "есть новая версия: %s (у меня %s)", tag, cur);
        if (auto_download) {
            xSemaphoreTake(s_state_mux, portMAX_DELAY);
            s_status.state = OTA_STATE_DOWNLOADING;
            xSemaphoreGive(s_state_mux);
            esp_err_t err = ota_from_url(bin_url);
            if (err != ESP_OK) {
                xSemaphoreTake(s_state_mux, portMAX_DELAY);
                s_status.state = OTA_STATE_ERROR;
                snprintf(s_status.error_message, sizeof(s_status.error_message),
                         "OTA не удалось: %s", esp_err_to_name(err));
                xSemaphoreGive(s_state_mux);
            }
        } else {
            xSemaphoreTake(s_state_mux, portMAX_DELAY);
            s_status.state = OTA_STATE_UPDATE_AVAILABLE;
            snprintf(s_status.available_version, sizeof(s_status.available_version), "%s", tag);
            snprintf(s_status.asset_url, sizeof(s_status.asset_url), "%s", bin_url);
            xSemaphoreGive(s_state_mux);
        }
    } else if (bin_url) {
        ESP_LOGI(TAG, "версия актуальна: %s (последний релиз %s)", cur, tag);
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_UP_TO_DATE;
        s_status.available_version[0] = '\0';
        s_status.asset_url[0] = '\0';
        xSemaphoreGive(s_state_mux);
    } else {
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "в релизе нет ассета для %s", CONFIG_IDF_TARGET);
        xSemaphoreGive(s_state_mux);
    }
    cJSON_Delete(json);
}

static void ota_download_task(void *arg)
{
    char *url = arg;
    esp_err_t err = ota_from_url(url);
    // При успехе ota_from_url не возвращает — устройство перезагружается.
    // Если всё же дошли сюда, обновляем статус ошибкой.
    if (err != ESP_OK) {
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "OTA не удалось: %s", esp_err_to_name(err));
        xSemaphoreGive(s_state_mux);
    }
    free(url);
    vTaskDelete(NULL);
}

static void ota_task(void *arg)
{
    for (;;) {
        ota_check_once(true);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ZVUK_OTA_CHECK_INTERVAL_MIN * 60 * 1000));
    }
}

void ota_update_get_status(ota_status_t *out)
{
    if (!s_state_mux || !out) {
        return;
    }
    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_state_mux);
}

esp_err_t ota_update_check_now(void)
{
    if (!s_state_mux) {
        return ESP_FAIL;
    }
    ota_check_once(false);
    return ESP_OK;
}

esp_err_t ota_update_start_download(void)
{
    if (!s_state_mux) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    if (s_status.state == OTA_STATE_CHECKING || s_status.state == OTA_STATE_DOWNLOADING) {
        xSemaphoreGive(s_state_mux);
        ESP_LOGW(TAG, "OTA уже выполняется");
        return ESP_FAIL;
    }
    if (s_status.asset_url[0] == '\0') {
        xSemaphoreGive(s_state_mux);
        ESP_LOGW(TAG, "URL обновления неизвестен, сначала проверьте релиз");
        return ESP_FAIL;
    }

    char *url = strdup(s_status.asset_url);
    if (!url) {
        xSemaphoreGive(s_state_mux);
        return ESP_FAIL;
    }
    s_status.state = OTA_STATE_DOWNLOADING;
    s_status.error_message[0] = '\0';
    xSemaphoreGive(s_state_mux);

    if (xTaskCreate(ota_download_task, "ota_download", 8192, url, 4, NULL) != pdPASS) {
        free(url);
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "не удалось запустить задачу OTA");
        xSemaphoreGive(s_state_mux);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ota_update_start(void)
{
    s_state_mux = xSemaphoreCreateMutex();
    if (!s_state_mux) {
        ESP_LOGE(TAG, "не удалось создать мьютекс OTA");
        return;
    }
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.current_version, sizeof(s_status.current_version),
             "%s", esp_app_get_description()->version);

    ESP_LOGI(TAG, "проверяю обновления в %s каждые %d мин",
             CONFIG_ZVUK_OTA_REPO, CONFIG_ZVUK_OTA_CHECK_INTERVAL_MIN);
    xTaskCreate(ota_task, "ota", 8192, NULL, 4, NULL);
}
