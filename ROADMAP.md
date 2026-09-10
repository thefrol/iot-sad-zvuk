# Дорожная карта iot-sad-zvuk

Цель: сетевые колонки на ESP32-C3, которыми управляют из интернета —
что играть, на каком устройстве, с какой громкостью. Аудио только
стримится, локально не хранится. Прошивка обновляется по воздуху (OTA).

## Железо и ограничения

- ESP32-C3, флеш **4 МБ** (подтверждено), PSRAM нет, SRAM 400 КБ.
- Звук: I2S → MAX98357A, декодер Helix (целочисленный) — у C3 нет FPU.
- Прошивка ~1.6 МБ (Wi-Fi + HTTP + MQTT, без TLS); слот 1.94 МБ, запас ~17%.
- Стриминг: HTTP(S)-ответ читается чанками → ring-буфер 32 КБ в RAM →
  `esp_audio_simple_dec` → I2S. Локальное хранилище не нужно.

## Архитектура (целевая)

**Устройство:**
- Wi-Fi station + `esp-mqtt` (пока plain TCP, TLS к этапу 6), топики:
  `zvuk/{id}/cmd` (beep / stop / `vol N` / `play <url>`),
  `zvuk/{id}/state` (retained статус + LWT «офлайн»),
  ota-топик добавить на этапе 5. `{id}` = `zvuk-XXXXXX` (3 байта MAC).
- Плеер: HTTP(S)-стрим → декодер → I2S; зацикливание = повторный запрос
  по EOF.
- OTA через `esp_https_ota` + откат
  (`esp_ota_mark_app_valid_cancel_rollback`).
- Таблица разделов (4 МБ): nvs + otadata + два app-слота по ~1.6 МБ +
  LittleFS ~700 КБ (запасной звук при потере сети, сертификаты).

**Сервер (Kubernetes):**
- MQTT-брокер (Mosquitto/EMQX) с TLS и авторизацией per-device.
- Бэкенд: реестр устройств, API «проиграй X на устройстве Y» → паблит в
  MQTT, раздаёт аудиофайлы по HTTP(S).
- Фронтенд: список устройств, выбор трека, play/loop/громкость/stop,
  запуск OTA.

## Этапы

1. ~~Исследование: флеш, размеры, выбор схемы~~ — готово (2026-09-09).
2. ~~4 МБ в конфиге + локальное управление по HTTP~~ — готово
   (Wi-Fi station, esp_http_server: `POST /play` стримит тело запроса
   в декодер, `GET /beep`, `POST /stop`, `?vol=`).
3. ~~HTTP-стриминг **с сервера по URL**~~ — готово (2026-09-09):
   `POST /play?url=...` и MQTT-команда `play <url>` через `esp_http_client`,
   стрим в тот же кольцевой буфер. Пока только plain HTTP, HTTPS — позже.
4. ~~MQTT control-plane~~ — готово (2026-09-09): Mosquitto в кластере,
   plain TCP (TLS — к этапу 6). Топики `zvuk/<id>/cmd` (beep / stop /
   `vol N` / `play <url>`) и retained `zvuk/<id>/state` + LWT.
   Device id: `zvuk-XXXXXX` (3 байта MAC). Прошивка ~1.6 МБ.
5. ~~OTA через GitHub~~ — готово (2026-09-10): `esp_https_ota` + откат
   (`esp_ota_mark_app_valid_cancel_rollback` после получения IP), фоновая
   проверка `releases/latest` раз в час, ручной триггер `POST /ota` и
   MQTT-команды `ota` / `ota check`. CI (`.github/workflows/release.yml`)
   по тегу `v*` собирает прошивку в контейнере `espressif/idf:v6.0.2` и
   публикует релиз с `iot-sad-zvuk-esp32c3.bin`; версия = тег
   (`-DPROJECT_VER`). Креды в CI — Secrets WIFI_SSID/WIFI_PASSWORD/MQTT_*.
   LittleFS пока не добавлен — встроенный `sound.mp3` остаётся.
6. Бэкенд + фронтенд на k8s, привязка устройств, TLS/авторизация
   (MQTT пока на общем логине `device`, без TLS).
   Частично готово (2026-09-10): zvuk-server (Go) + фронт на
   https://zvuk.devdima.ru, реестр устройств, upload MP3, команды.
   Осталось: привязка/имена устройств, TLS на MQTT, авторизация.
7. Галерея звуков + коллекции: загруженные MP3 уже сохраняются в PVC
   (`/data`, 1Gi) — добавить `GET /api/audio` (список файлов), повторное
   воспроизведение из галереи без повторной загрузки, имена/названия треков,
   группировка в коллекции (плейлисты), удаление. Во фронте — секция
   «Галерея» с кнопкой play у каждого трека.
8. Зацикливание: команда `loop <url>` (или `play <url> loop`) — прошивка
   повторно запрашивает URL по EOF (схема уже заложена в архитектуру),
   `stop` выходит из цикла. Во фронте — переключатель «зациклить».
   Нюанс: MP3-стрим с нуля каждый круг → короткая пауза между кругами,
   для бесшовного лупа позже смотреть на кеширование PCM.
9. CI/CD — деплой по коммиту (GitHub Actions):
   - `iot-sad-zvuk` (push в `server/`): docker build → push в registry
     кластера (`5.183.191.188:32000`) → `kubectl apply` манифестов +
     rollout restart. Реестр сейчас insecure (plain HTTP) — из GH Actions
     надо либо поднять ingress/TLS на registry, либо пушить через SSH-туннель
     на worker, либо переехать на GHCR + imagePullSecret.
   - kubectl-доступ из CI: scoped kubeconfig через
     `task new-client NAME=iot-sad-zvuk` в devdima-k8s (уже есть схема,
     clients/), положить в GitHub Secrets как `KUBECONFIG_B64`.
   - Манифесты живут в devdima-k8s → либо workflow там (тогда сборка
     образа триггерится repository_dispatch'ем из iot-sad-zvuk), либо
     перенести манифесты приложения в iot-sad-zvuk/deploy/ (broker и
     инфра остаются в devdima-k8s). Решить на этапе реализации.
   - Сборка прошивки в CI — часть этапа 5 (OTA).

## Известные проблемы

- Фронтенд открывается только по https://zvuk.devdima.ru/static/,
  на `/` — не работает (проверить роутинг статики в zvuk-server:
  embed + http.Handle, вероятно index отдаётся не на корне).

## Безопасность (к этапу 5–6)

- MQTTS (8883) с per-device логином/паролем или client certs.
- OTA только по HTTPS с проверкой сертификата (crt bundle).
- Опционально secure boot v2 + flash encryption (необратимо, eFuse) —
  решить перед «уходом в прод».
