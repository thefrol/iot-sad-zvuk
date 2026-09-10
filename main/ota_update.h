#pragma once

#include "esp_err.h"

// OTA-обновление прошивки с GitHub Releases.
// Фоновая задача опрашивает releases/latest репозитория CONFIG_ZVUK_OTA_REPO,
// сравнивает версию релиза со своей и при появлении новой скачивает
// .bin-ассет под свой чип через esp_https_ota и перезагружается.

typedef enum {
    OTA_STATE_IDLE,             // нет активной операции
    OTA_STATE_CHECKING,         // идёт проверка релиза
    OTA_STATE_UPDATE_AVAILABLE, // доступно обновление (ручная проверка)
    OTA_STATE_DOWNLOADING,      // идёт скачивание/запись образа
    OTA_STATE_REBOOT_PENDING,   // образ записан, скоро перезагрузка
    OTA_STATE_UP_TO_DATE,       // текущая прошивка актуальна
    OTA_STATE_ERROR,            // последняя операция завершилась ошибкой
} ota_state_t;

// Публичное состояние OTA. Копируется под мьютексом в ota_update_get_status().
typedef struct {
    ota_state_t state;
    char current_version[32];
    char available_version[32]; // пустая строка, если обновления нет
    char asset_url[512];        // URL .bin при наличии обновления
    char error_message[128];    // последнее сообщение об ошибке
} ota_status_t;

// Запускает фоновую задачу проверки обновлений.
// Вызывать после установления Wi-Fi (нужен интернет).
void ota_update_start(void);

// Потокобезопасно копирует текущее состояние OTA в *out.
void ota_update_get_status(ota_status_t *out);

// Синхронно проверить последний релиз на GitHub, сравнить версии и
// запомнить URL ассета. НЕ скачивает и НЕ перезагружает.
// Блокирует вызывающую задачу на время HTTP-запроса (~1–3 с).
// Возвращает ESP_OK, если проверка выполнена (даже если обновления нет).
esp_err_t ota_update_check_now(void);

// Запустить скачивание и установку запомненного обновления в фоновой задаче.
// Возвращает ESP_OK, если задача запущена; скачивание и перезагрузка
// происходят асинхронно. ESP_FAIL — уже идёт операция или URL неизвестен.
esp_err_t ota_update_start_download(void);
