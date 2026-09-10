---
name: firmware-release
description: Выпуск новой прошивки iot-sad-zvuk и доставка её на устройства по OTA. Использовать, когда нужно опубликовать обновление прошивки, поднять версию, создать тег/релиз или проверить, что устройство обновилось.
---

# Выпуск прошивки (release) и OTA-обновление устройств

Схема: тег `v*` в GitHub → Actions собирает прошивку → публикует релиз с
`iot-sad-zvuk-esp32c3.bin` → устройства сами скачивают его по HTTPS
(поллинг раз в час) либо по принудительной команде.

## Правила (нарушать нельзя)

- **Тег = `project(... VERSION)` в корневом `CMakeLists.txt` = версия релиза.**
  Сначала поднимаем VERSION, коммитим, потом тег. CI передаёт тег как
  `-DPROJECT_VER`, поэтому VERSION в репозитории нужен в первую очередь
  для локальных сборок и порядка.
- Релизный бинарь собирается ТОЛЬКО в CI: там зашиваются Wi-Fi/MQTT креды
  из GitHub Secrets. Локальная сборка не для дистрибуции.
- Если релиз уже вышел, следующую прошивку выпускать новым тегом с версией
  выше — устройства сравнивают semver и не обновятся на ту же версию.

## Цикл выпуска

```bash
cd <корень репозитория>

# 1. Поднять версию
#    CMakeLists.txt: project(iot-sad-zvuk VERSION X.Y.Z)

# 2. Локальная сборка — проверить, что собирается и влезает в слот:
. ~/esp/esp-idf/export.sh
idf.py build
#    В выводе: "binary size 0x... Smallest app partition is 0x1f0000" —
#    должно оставаться свободное место.

# 3. Коммит и тег
git add -A && git commit -m "..."
git tag vX.Y.Z
git push origin main
git push origin vX.Y.Z        # тег — ОТДЕЛЬНЫМ пушем
```

Почему тег отдельно: замечено, что при `git push origin HEAD --tags`
(ветка и новый тег одной командой) GitHub иногда не запускает workflow.
Проверка, что ран стартовал: `gh run list -R thefrol/iot-sad-zvuk --limit 3`.
Если через минуту пусто — пересоздать тег:
`git push origin :refs/tags/vX.Y.Z && git push origin vX.Y.Z`.

## 4. Дождаться CI и релиза

```bash
gh run watch -R thefrol/iot-sad-zvuk        # ~3-4 минуты
gh release view vX.Y.Z -R thefrol/iot-sad-zvuk
# в ассетах должен быть iot-sad-zvuk-esp32c3.bin
```

## 5. Обновление устройств

- Автоматически: в течение часа (интервал `CONFIG_ZVUK_OTA_CHECK_INTERVAL_MIN`).
- Принудительно по локальной сети: `curl -X POST http://<IP>/ota`
  (ответ «скачиваю vX.Y.Z, устройство перезагрузится»).
- Принудительно из интернета: MQTT-команда `ota` в топик `zvuk/<id>/cmd`
  (`ota check` — только проверить наличие, не качать).

## 6. Проверить, что обновилось

- `curl http://<IP>/ota` → `"current":"X.Y.Z"`, `state:"up_to_date"`.
- Поле `fw` в retained `zvuk/<id>/state`.
- Лог порта: `есть новая версия` → `Writing to <ota_N>` → ребут →
  `App version: X.Y.Z` → `версия актуальна`.
- Обязательно проверить, что звук играет: `curl http://<IP>/beep`.

## Если что-то пошло не так

- Устройство не загрузилось на новой прошивке → загрузчик сам откатит
  на предыдущий слот (включён `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`;
  самотест — `esp_ota_mark_app_valid_cancel_rollback()` после Wi-Fi).
- `HTTP 403` от api.github.com — нет User-Agent (не ломать) или исчерпан
  rate limit 60 запр/час на IP.
- `HTTP 404` от releases/latest — релиза ещё нет (CI не завершён?).
- `Out of buffer` при скачивании — кто-то уменьшил буферы esp_http_client
  в `ota_from_url()` (нужно 2048 из-за длинного редиректного URL GitHub).
- `PSA signature verification failed -141` — не хватило кучи на TLS
  (см. граблю №11 в AGENTS.md): не держать большие буферы во время
  рукопожатия.
- Устройство обновилось и потеряло сеть — в CI-сборку не попали креды:
  проверить секреты WIFI_SSID/WIFI_PASSWORD/MQTT_URI/MQTT_USERNAME/
  MQTT_PASSWORD в репозитории.
