# iot-sad-zvuk

IoT-колонка на ESP32-C3: прошивка на ESP-IDF, звук через I2S-усилитель MAX98357A
и 3W динамик. Устройство поднимает Wi-Fi station + HTTP-сервер и играет MP3,
присланный в теле `POST /play` или лежащий по URL (стримится прямо в декодер,
локально не хранится). Управление по MQTT (команды/статус), OTA-обновление
с GitHub Releases. Дальше (TLS, серверная часть) — в `ROADMAP.md`. Флеш 4 МБ.

## Железо

- Плата: ESP32-C3 (USB — родной USB-Serial-JTAG, порт вида `/dev/cu.usbmodem*`)
- Усилитель: модуль MAX98357A (I2S, класс D), распиновка в стиле Adafruit —
  пины **SD и GAIN разведены отдельно**
- Динамик: 3W, подключён к клеммам `+`/`−` усилителя

### Распиновка ESP32-C3 ↔ MAX98357A

| MAX98357A | ESP32-C3 | Примечание |
|---|---|---|
| BCLK | GPIO1 | |
| LRC (WS) | GPIO3 | |
| DIN | GPIO4 | |
| VIN | 5V | не 3.3V — будет очень тихо |
| GND | GND | общий GND обязателен |
| SD | в воздухе | на плате подтянут к питанию = усилитель включён. К GND = тишина (shutdown) |
| GAIN | VDD | см. таблицу ниже |

### Пин GAIN (громкость усилителя)

| Подключение GAIN | Усиление |
|---|---|
| Не подключён | 9 дБ (по умолчанию) |
| К GND | 3 дБ |
| Через 100 кОм к GND | 6 дБ |
| Через 100 кОм к VDD | 12 дБ |
| Напрямую к VDD | 15 дБ (максимум) |

> Была реальная бага: полная тишина из-за случайно посаженного на землю GAIN/SD.
> При отладке тишины сначала проверять питание VIN и SD/GAIN.

Пины I2S задаются в `main/main.c` (`I2S_BCLK_GPIO` и др.). GPIO1/3/4 на C3 —
обычные пины, без ограничений. GPIO2 — strapping-пин, лучше не использовать.

## Софт

- **ESP-IDF v6.0.2** (`~/esp/esp-idf`, активировать: `source ~/esp/esp-idf/export.sh`)
- Звук: I2S Philips 16-bit stereo, 44100 Гц, новый драйвер `driver/i2s_std.h`
- MP3-декодер: **Helix (целочисленный)** через managed-компонент
  `espressif/esp_audio_codec`, API `esp_audio_simple_dec` (сам парсит кадры —
  скармливаем сырые данные кусками). В Kconfig компонента выключены все
  декодеры/энкодеры, кроме MP3 (`sdkconfig.defaults`): по умолчанию там
  линкуются LC3/AMR/Opus/SBC/ALAC/... и прошивка тяжелеет на ~560 КБ
- Файл `main/sound.mp3` (короткий бип: синус 880 Гц, 0.35 с с fade-in/out,
  44.1 кГц стерео 64 кбит/с) вшит во флеш через `EMBED_FILES` — играется
  по `GET /beep` для проверки железа
- Wi-Fi station: SSID/пароль в Kconfig (`idf.py menuconfig` → iot-sad-zvuk,
  `CONFIG_IOT_WIFI_SSID`/`CONFIG_IOT_WIFI_PASSWORD`; реальные значения лежат
  только в локальном `sdkconfig` — он в .gitignore)
- MQTT control-plane (managed-компонент `espressif/mqtt` — в IDF 6 из дерева
  компонентов вынесен): Kconfig `CONFIG_IOT_MQTT_URI` / `_USERNAME` /
  `_PASSWORD` (пустой URI = MQTT не стартует). Стартует после получения IP.
  Device id = `zvuk-XXXXXX` (последние 3 байта MAC). Топики: подписка
  `zvuk/<id>/cmd` (QoS 1), retained-стейт `zvuk/<id>/state`
  (`{"online","id","ip","playing","volume","fw"}`, LWT = `{"online":false}`).
  Команды plain-text: `beep`, `stop`, `vol <0-255>`, `play <url>`,
  `loop <url>` (играет по кругу, каждый круг URL запрашивается заново;
  выход по `stop`), `ota` (проверить релиз и обновиться), `ota check`
  (только проверить);
  неизвестные логируются и игнорируются. Команды исполняет задача `mqtt_cmd`
  (8 КБ стека — внутри бывает esp_http_client), но `stop` обрабатывается
  прямо в хендлере MQTT-событий, иначе не прервать воспроизведение, пока
  задача занята.
- HTTP API (`esp_http_server`):
  - `POST /play` — тело запроса это MP3-поток; хендлер кладёт данные в
    кольцевой буфер (32 КБ), декодирует отдельная задача `player` (16 КБ стека),
    перед стартом ждёт предбуфер 8 КБ; ответ приходит по окончании
  - `POST /play?url=...` — устройство само забирает MP3 по HTTP
    (`esp_http_client`) в тот же кольцевой буфер (общий каркас —
    `stream_session_run`, источники: `httpd_body_read` / `http_url_read`)
  - `GET /beep` — встроенный звук один раз
  - `POST /stop` — остановить
  - `GET /ota` — статус OTA (JSON: state/current/available/error)
  - `POST /ota` — принудительно проверить релиз и обновиться, если есть
    новее (проверка синхронная, идёт в задаче httpd — стек поднят до 8 КБ
    из-за TLS-рукопожатия)
  - `?vol=0..255` — громкость (параметр `/play` и `/beep`, дефолт 64)
- OTA с GitHub Releases (`main/ota_update.c`, схема перенесена из
  ebbflow-lamp): раз в час (`CONFIG_ZVUK_OTA_CHECK_INTERVAL_MIN`) опрашивает
  `api.github.com/repos/<CONFIG_ZVUK_OTA_REPO>/releases/latest`, сравнивает
  semver-тег со своей версией и при наличии новой тянет ассет
  `iot-sad-zvuk-<chip>.bin` через `esp_https_ota` (cert bundle) и ребутится.
  Критично: буферы esp_http_client 2048 байт (редиректный URL GitHub
  ~900+ байт не влезает в дефолтные 512), у GitHub API обязателен заголовок
  User-Agent, rate limit 60 запр/час на IP — отсюда интервал 60 мин.
  Откат: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, самотест —
  `esp_ota_mark_app_valid_cancel_rollback()` в `app_main` после получения IP.
- Релиз — тегом: `git tag vX.Y.Z && git push origin vX.Y.Z` → CI
  (`.github/workflows/release.yml`, контейнер `espressif/idf:v6.0.2`) собирает
  прошивку и публикует релиз с `iot-sad-zvuk-esp32c3.bin`. Версия прошивки =
  тег (CI передаёт `-DPROJECT_VER`); не забывать поднимать
  `project(... VERSION x.y.z)` в корневом `CMakeLists.txt` под новый тег.
  Креды для CI-сборки — GitHub Secrets: `WIFI_SSID`, `WIFI_PASSWORD`,
  `MQTT_URI`, `MQTT_USERNAME`, `MQTT_PASSWORD` (зашиваются в бинарь, без них
  устройство после OTA потеряет сеть/брокер).
  **Полный цикл выпуска со всеми граблями — скилл
  `.agents/skills/firmware-release`.**
- Параллельное воспроизведение запрещено мьютексом (`409 Conflict`).
- Оптимизация компилятора — `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`):
  на `-Og` декодер едва поспевает за реальным временем при активном Wi-Fi.

### Важно: регистрация декодеров — ДВА слоя

`esp_audio_simple_dec_register_default()` регистрирует только контейнерные
парсеры (WAV/M4A/TS/OGG). Сами кодеки (MP3, AAC...) регистрируются отдельно —
`esp_audio_dec_register_default()`. Нужны **оба** вызова, иначе
`esp_audio_simple_dec_open` вернёт `-7` (not supported). См. `app_main()`.

## Грабли, которые уже прошли (не наступать повторно)

1. **У ESP32-C3 нет FPU.** Декодер minimp3 (float) декодировал в 4–5 раз
   медленнее реального времени → DMA недогружался, ритмичные «дыгы» вместо
   звука. На C3 только целочисленные декодеры (Helix, libmad).
2. **Стек декодера.** В задаче с дефолтным стеком (3.5–4 КБ) декодер падает со
   Stack protection fault. Декодирование идёт в отдельной задаче `player`
   со стеком 16 КБ; задача HTTP-хендлера декодер не трогает.
3. **IDF 6: компонент называется `log`, не `esp_log`.** Если указать
   `PRIV_REQUIRES`, дефолтный список common-компонентов отключается — зависимости
   надо перечислять явно (`esp_driver_i2s`, `esp_driver_gpio`, `log`, ...).
4. `idf.py monitor` из скриптов не работает (требует TTY). Читать порт через
   pyserial: `s.setDTR(False); s.setRTS(True); sleep(0.15); s.setRTS(False)` —
   аппаратный сброс (DTR=False обязательно, иначе плата уйдёт в download-режим
   и логов не будет).
5. `say` + `ffmpeg` на macOS генерируют рабочий MP3:
   `say -v Milena -o /tmp/zvuk.aiff "текст" && ffmpeg -i /tmp/zvuk.aiff -ar 44100 -ac 2 -b:a 64k main/sound.mp3`.
   Текущий бип сделан чистым ffmpeg:
   `ffmpeg -f lavfi -i "sine=frequency=880:duration=0.35" -af "afade=t=in:st=0:d=0.015,afade=t=out:st=0.20:d=0.15,volume=0.6" -ar 44100 -ac 2 -b:a 64k main/sound.mp3`
6. **I2S крутит хвост.** Когда запись в DMA заканчивается, канал зацикливает
   последние данные — слышно «пек-пек-пек» (~185 мс при нашем буфере). Лечится
   заливкой нулей на весь DMA-буфер после воспроизведения (`i2s_flush_silence`).
   Два нюанса: резкий переход к нулям слышен как щелчок — поэтому сначала
   короткий fade-out (~12 мс) от последнего сэмпла; и декодеру надо шлёпнуть
   пустой кадр с `eos=true` в конце стрима, иначе он закрывается с недоигранным
   последним MP3-кадром (тоже щелчок).
7. **Wi-Fi power save = заикания.** По умолчанию станция засыпает между
   биконами (DTIM) — recv встаёт до ~300 мс, звук дёргается. Обязательно
   `esp_wifi_set_ps(WIFI_PS_NONE)`. Ещё: синхронная цепочка recv→decode→I2S
   заикалась на слабом сигнале — поэтому сеть и декодер развязаны кольцевым
   буфером (StreamBuffer 32 КБ + предбуфер 8 КБ).
8. **IDF 6: MQTT — managed-компонент.** В `~/esp/esp-idf/components/mqtt`
   остались только test_apps; сам клиент подключается как `espressif/mqtt`
   в `idf_component.yml` (примеры IDF пинуют `^1.0.0`), в CMake — `mqtt`
   в PRIV_REQUIRES.
9. **esp_http_server однопоточный.** Пока `play_handler` ждёт конца
   воспроизведения, остальные HTTP-запросы (включая `POST /stop`!) стоят в
   очереди — остановить HTTP-сессию по HTTP нельзя, только по MQTT (`stop`
   там обрабатывается вне очереди команд). 409 по HTTP поймать почти
   нереально: запросы сериализуются раньше мьютекса — мьютекс реально
   защищает гонку HTTP vs MQTT.
10. **Retained-стейт публикуем QoS 0, не QoS 1.** На плохом линке (RSSI
    −86) QoS1-ретрансляции приходили на брокер с опозданием и затирали
    retained-стейт устаревшим `playing:true` — свежая подписка видела
    «играет» на молчащем устройстве.
11. **OTA/TLS: не держать большой буфер во время TLS-рукопожатия.** Свободной
    кучи после загрузки ~65 КБ (стрим-буфер 32 КБ + стеки задач + Wi-Fi);
    mbedTLS съедает ~50 КБ. В ebbflow-lamp буфер ответа GitHub API (32 КБ)
    выделялся ДО рукопожатия — у нас это не влезало, и верификация
    сертификата падала с загадочным `PSA signature verification failed
    -141` (= INSUFFICIENT_MEMORY). Лечение: буфер ответа выделяется лениво
    на первом HTTP_EVENT_ON_DATA и уменьшен до 16 КБ (JSON релиза реально
    ~6 КБ). Минимум кучи в пике OTA-скачивания ~23 КБ — норма, но новые
    большие статические/heap-буферы могут это сломать.

## Сборка и прошивка

```bash
source ~/esp/esp-idf/export.sh
idf.py menuconfig             # iot-sad-zvuk → Wi-Fi SSID/пароль (один раз)
idf.py build flash            # порт подхватится сам, или: idf.py -p /dev/cu.usbmodemXXXX flash
```

Проверка (IP смотреть в логе порта): `curl http://<IP>/beep`,
`curl -X POST --data-binary @sound.mp3 http://<IP>/play`.

Отладка «нет звука» (мультиметр, режим DC, на ножках модуля усилителя):
BCLK ~1.5–1.7V (меандр 1.4 МГц), LRC ~1.5–1.7V, DIN ~0.5–1.6V. Нули = обрыв.

## Следующие шаги

Полная дорожная карта — в `ROADMAP.md`. Ближайшее:
- TLS для MQTT (сейчас plain TCP — домашняя демка) и HTTPS для `play <url>`
- LittleFS-раздел (запасной звук при потере сети), потом убрать встроенный
  `sound.mp3`
- Довести сервер (этап 6 ROADMAP): привязка/имена устройств, TLS на MQTT,
  авторизация; коллекции/плейлисты в галерее
- Мут по сети: SD-пин усилителя можно посадить на GPIO

## Структура

- `main/main.c` — вся прошивка: I2S init, Wi-Fi station, HTTP API
  (`/play` в т.ч. `?url=`, `/ota`), MQTT control-plane, декодер
- `server/` — zvuk-server (Go): реестр устройств по MQTT-стейтам,
  команды (`POST /api/devices/{id}/cmd`, `/play`), галерея звуков
  (`GET/POST /api/audio`, `POST /api/audio/{name}/play` — тело
  `{"device","loop"}`, `PATCH`/`DELETE /api/audio/{name}`; метаданные в
  `tracks.json` рядом с mp3 в DATA_DIR), раздача mp3 `GET /audio/{name}`,
  фронт из embed-статики (`static/index.html`, отдаётся на `/`)
- `main/ota_update.c` — OTA: фоновая проверка GitHub Releases, скачивание
  через `esp_https_ota`, ручной триггер (`ota_update_check_now` /
  `ota_update_start_download`)
- `main/Kconfig.projbuild` — Wi-Fi кредешелы, MQTT URI/логин/пароль,
  OTA-репозиторий и интервал проверки (menuconfig → iot-sad-zvuk)
- `main/sound.mp3` — встроенный звук для `/beep` (вшивается во флеш)
- `main/idf_component.yml` — зависимости `espressif/esp_audio_codec`,
  `espressif/mqtt`, `espressif/cjson` (в IDF 6 cJSON вынесен из ядра)
- `.github/workflows/release.yml` — CI: сборка прошивки по тегу `v*` и
  публикация GitHub Release с бинарником
- `.github/workflows/server-image.yml` — CI/CD сервера: пуш в `main` с
  изменениями в `server/`/`deploy/` или тег `server-v*` → Docker-образ в
  GHCR (`ghcr.io/<owner>/zvuk-server`, теги latest/sha/версия) → деплой в
  Kubernetes: `kubectl apply -f deploy/` + rollout (на теге —
  `kubectl set image` на версию). kubectl-доступ — секрет `KUBECONFIG_B64`
  (scoped kubeconfig, namespace-admin `iot-sad-zvuk`; выдан
  `task new-client NAME=iot-sad-zvuk` в репе devdima-k8s, локальная копия —
  `~/Dimba/devdima-k8s/clients/iot-sad-zvuk/kubeconfig`)
- `deploy/` — манифесты Kubernetes (namespace `iot-sad-zvuk`): mosquitto
  (MQTT-брокер, NodePort 31883) и zvuk-server (deployment/service/ingress/
  PVC). Переехали сюда из репо devdima-k8s 2026-09-23; применяются CI
- `partitions.csv` — два OTA-слота по ~1.94 МБ (app ~1.19 МБ — MP3-only
  кодек + TLS/OTA, запас ~41%); места под LittleFS теперь бы хватило,
  но разметку не трогаем до этапа с LittleFS
- `.agents/skills/firmware-release/` — скилл: цикл выпуска прошивки и OTA
  (поднять VERSION → тег отдельным пушем → CI → проверка на устройстве)
- `ROADMAP.md` — целевая архитектура и план (TLS, сервер)
- `managed_components/` — скачанные компоненты (в .gitignore)
