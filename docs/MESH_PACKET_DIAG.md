# Диагностика mesh-пакетов (ESP и T114)

Операциональный чеклист по расследованию потерь и искажений LoRa mesh при совпадающих билдах прошивки. Исходная методика — внутренний план диагностики; здесь — исполняемые шаги и ссылки на код.

**См. также:** [`PROTOCOL.md`](PROTOCOL.md), [`API.md`](API.md), [`firmware/src/faketec/README.md`](../firmware/src/faketec/README.md).

---

## 1. Зафиксировать симптом (`define-symptom`)

Заполнить перед расследованием:

| Вопрос | Варианты |
|--------|----------|
| Где видна проблема? | Эфир (UART: RX/PARSE) / только BLE JSON / оба |
| Направление | Односторонняя связь / обе стороны |
| Характер | Тишина RX / `RX_PARSE_FAIL` / `len_mismatch` / парс OK, но нет в приложении |
| Масштаб | Редкие потери vs «всё подряд» |

В логах указывать версию прошивки (`firmware/src/version.h`), git hash билда при отчёте.

---

## 2. Версии и контракт пакета (`verify-protocol-version`)

- При **одинаковых** билдах не считать первопричиной «рассинхрон PROTOCOL», пока не доказано иное.
- При сомнении: байт `Ver` в заголовке кадра на эфире; сравнение с `protocol::` в `firmware/src/protocol/packet.cpp`.
- **Автопроверка формата на хосте:**

```text
cd firmware
pio test -e native
```

Собирается только `protocol/packet.cpp` + Unity-тесты в `firmware/test/test_packet.cpp` (см. `[env:native]` в `firmware/platformio.ini`).

---

## 3. Семантика доставки (`delivery-semantics`)

- Mesh в RiftLink **не** TCP: best-effort, ACK/ретрансляция зависят от opcode, TTL, маршрута, очередей.
- Источники: `docs/PROTOCOL.md`; код `firmware/src/routing/`, `firmware/src/msg_queue/`, `firmware/src/frag/`, `firmware/src/offline_queue/`, `firmware/src/ack_coalesce/`.
- Зафиксировать **тип трафика** (HELLO, MSG, фрагменты, relay и т.д.) — от него зависит ветка.

---

## 4. Матрица симптомов (`classify-symptom-matrix`)

| Наблюдение | Куда смотреть первым |
|------------|----------------------|
| Часто `PARSE` / `len_mismatch` / короткие кадры | Эфир (SNR/RSSI), коллизии, SF/BW/CR, обрезка RX, антенна; на T114 — TCXO/DIO2 |
| Заголовок OK, крипта/полезная нагрузка нет | `firmware/src/crypto/`, `firmware/src/x25519_keys/`, порядок KEY_EXCHANGE |
| Парс OK, в приложении нет | BLE, очереди, маршрут, TTL — не путать с «битым» кадром |
| Одинаково плохо ESP и nRF в одной сцене | Общий `protocol/`, общая логика `handlePacket` / `handle_packet_nrf`, эфир |

---

## 5. Радио-паритет (`radio-parity`)

- Один регион, канал, **SF / BW / CR**, пресет модема: `firmware/src/region/`, на nRF — `firmware/src/faketec/region_nrf.cpp`; см. `firmware/src/radio/radio.h` (`requestModemPreset`, фактический SF в логах PARSE).
- **T114 / nrf52:** `firmware/platformio.ini` (`heltec_t114` / `nrf52_base`): `LORA_MODULE_HAS_TCXO`, `LORA_DIO2_RF_SWITCH`; несоответствие модулю — см. `firmware/src/faketec/README.md`.
- В `handlePacket` при плохом эфире возможны массовые PARSE без логического бага (congestion).

---

## 6. Сопоставление RX-путей (`compare-rx-paths`)

| Платформа | Цепочка | Диагностика в логе |
|-----------|---------|-------------------|
| ESP | RX → `radioSchedulerTask` / очередь → `packetTask` → `handlePacket` | `firmware/src/async_tasks.cpp` (`packetTask` → `handlePacket`); `firmware/src/main.cpp` — `RIFTLINK_DIAG("PARSE", ...)` |
| nRF | Loop в `faketec/main.cpp` → `handlePacket` (тело в `handle_packet_nrf_body.inc`) | `RIFTLINK_DIAG("MAIN", "event=RX_RAW ...")` в `firmware/src/faketec/main.cpp`; те же теги `PARSE` в `firmware/src/faketec/handle_packet_nrf_body.inc` |

Сравнивать: длина кадра, первые байты после `RX_RAW`, затем `RX_PARSE_FAIL` / успешный разбор.

---

## 7. Крипто-сессия (`crypto-session`)

Если заголовок парсится, но содержимое отбрасывается или «мусор»:

- Логи стадий KEY: на nRF — `KEY_RX_RAW`, `KEY_STORE_OK`, `KEY_RX_PARSE_FAIL` и т.д. в `handle_packet_nrf_body.inc`; на ESP — цепочка вокруг `handlePacket`, при необходимости `RIFTLINK_DIAG("BLE_CHAIN", ...)` (см. `main.cpp`).
- Сценарий: сброс ключей / порядок KEY_EXCHANGE — два узла согласованно.

---

## 8. Двухузловой минимум (`two-node-repro`)

1. Одинаковая прошивка, один регион, стационарные антенны.
2. Один тип кадра (например HELLO или PING по протоколу).
3. TX: лог после `buildPacket` (длина, префикс).
4. RX: `RX_RAW` / `RX_PARSE_FAIL` / успешный вход в обработчик opcode.
5. Если байты совпадают, а логика «ломается» выше — искать пост-обработку, не эфир.

---

## 9. Отделение BLE (`ble-decouple`)

Если симптом только в приложении (обрыв JSON, нет evt):

- Прошивка: `firmware/src/ble/ble.cpp` (ESP), `firmware/src/faketec/ble_nrf.cpp` (nRF); на nRF возможен `RIFTLINK_DIAG("BLE", "event=RX_PARSE_FAIL ...")` для входящего JSON.
- Лимиты и контракт: `docs/API.md` (размер строки, `cmdId`, фрагментация notify).

---

## 10. Артефакты отчёта

- Фрагменты логов `PARSE` / `RX_RAW` с обеих сторон; счётчики успешных проходов `handlePacket` vs отказов парсера за сессию.
- Версия прошивки, регион, пресет модема, SF, RSSI/SNR если есть.
- Для T114 — env и флаги SX1262 из README.
- Таблица: **тип трафика** → поведение (потеря / битый парсер / нет до приложения).

---

## Порядок при одинаковых версиях (кратко)

1. Не ожидать end-to-end гарантию; уточнить тип сообщения и политику ACK/фрагментов.
2. Отделить битый парсер от полного отсутствия RX.
3. Эфир + двухузловой тест с логами TX/RX.
4. Если кадр на приёмнике целый — маршрут и очереди.
5. Если заголовок целый, полезная нагрузка не открывается — крипта/сессии.
6. BLE — только если сырой эфирный кадр в порядке, а проблема в приложении.
