# RiftLink для FakeTech V5 Rev B

Прошивка RiftLink для платы **FakeTech V5 Rev B** (NiceNano nRF52840 + HT-RA62/RA-01SH LoRa).

### Архитектура (паритет с Heltec V3, без Wi‑Fi)

- **Планирование:** один поток `loop()`, без отдельной radio-task как на ESP. Очередь TX и отложенные кадры — `async_tx` (`queueTxPacket`, `queueDeferredSend`, `queueDeferredAck`, **`queueDeferredRelay`**, **`relayHeard`**, `asyncTxPoll`).
- **Приём:** после `parsePacket` и ветки `OP_XOR_RELAY` — единый **`processRelayPipeline`** в `main.cpp` (как блок relay в ESP `handlePacket`): overhear/courier, XOR-пара, отложенный relay, dedup и лимиты SOS; не упрощённый «сразу `radio::send`».
- **Хранилище:** `storage` / InternalFS; мелкие ключи — `getBlob`/`setBlob` (до 64 B в RAM-кэше), крупные блобы — **`setLargeBlob`/`getLargeBlob`** (группы, offline). **Группы V2** — `groups_storage.cpp`, **offline_queue** — `offline_queue_storage.cpp`.
- **Портируемые исходники:** `routing_nrf.cpp`, `msg_queue_nrf.cpp`, `frag_nrf.cpp`, `pkt_cache_nrf.cpp`, `packet_fusion_nrf.cpp`, `ack_coalesce_nrf.cpp`, `network_coding_nrf.cpp` (общие заголовки в `src/<module>/`).
- **Фазы 5–8 (nRF):** `v3_aux_nrf.cpp` — **mab**, **collision_slots**, **beacon_sync**, **clock_drift** (логика как на ESP, без `esp_random`); **voice_frag** — минимальные заглушки `send`/`onFragment` (запись голоса не поддерживается).

## Дисплей и меню

OLED через `faketec/display.*` — минимальный слой (`clear`/`print`/`show`), **без** полноценного меню как на ESP (`ui/display.cpp`: HOME, разделы, popup). Паритет UX с Heltec — отдельный объём работ.

Общие модули рефакторинга UI (`firmware/src/ui/ui_scroll.h`, `ui_menu_exec.*`, `ui_layout_profile.h`, `ui_icons.h` и т.д.) подключаются только в ESP-сборках (`heltec_*`, `lilygo_*`); env **faketec_v5** собирает `faketec/` + `protocol/` и не тянет эти файлы.

## Сборка

```bash
cd firmware
pio run -e faketec_v5
```

- **`faketec_v5`** — **без BLE** (`-DRIFTLINK_SKIP_BLE` в `platformio.ini`): стабильный USB Serial + LoRa mesh.
- **`faketec_v5_ble`** — тот же код, но флаг `RIFTLINK_SKIP_BLE` снят (`build_unflags`): Nordic UART GATT; SoftDevice и реклама поднимаются в конце `setup()` (после стабилизации USB), без многосекундной задержки в `loop()`.

Артефакты: `.pio/build/<env>/firmware.hex`, `firmware.zip`

## Прошивка

1. **DFU-режим:** двойной клик по кнопке RST на NiceNano (в течение 0.5 с)
2. Появится диск `NICENANO` — скопируйте `firmware.zip` на диск (или используйте `nrfutil`)

Или через PlatformIO:
```bash
pio run -t upload -e faketec_v5
```

## Распиновка (board.h)

Соответствует **Meshtastic** `nrf52_promicro_diy_tcxo` (fakeTec PCB = DIY ProMicro). Логические пины 0..47 = GPIO nRF.

| Сигнал | Пин (GPIO) |
|--------|------------|
| LoRa NSS | 45 (P1.13) |
| LoRa RST | 9 (P0.09) |
| LoRa DIO1 | 10 (P0.10) |
| LoRa BUSY | 29 (P0.29) |
| LoRa SCK | 43 (P1.11) |
| LoRa MISO | 2 (P0.02) |
| LoRa MOSI | 47 (P1.15) |
| OLED SDA | 36 (P1.04) |
| OLED SCL | 11 (P0.11) |

Другая ревизия платы — сверьте схему и при необходимости поправьте `board.h`.

## Функции

- **LoRa mesh:** HELLO, MSG, ACK, relay (в т.ч. XOR / deferred relay), маршрутизация и очереди исходящих как на V3 (без Wi‑Fi)
- **Дисплей:** автоопределение I2C OLED (SSD1306 @ 0x3C)
- **Команды:** BLE GATT (UUID как в `docs/API.md`) и USB Serial — JSON, до 512 байт на запись
- **Совместимость:** тот же протокол, что и Heltec — узлы в одной сети

## Совместимость с Heltec V3 (приём / «вижу только в одну сторону»)

Параметры эфира должны совпадать с соседом, иначе декодирования не будет.

| Проверить | FakeTech | Heltec V3 |
|-----------|----------|-----------|
| Регион / частота | `evt:node` → `region`, `freq`, при EU ещё `channel` (0/1/2) | То же в приложении / NVS |
| Modem | `sf`, `bw`, `cr`, `modemPreset` в `evt:node` | Тот же пресет (0=Speed … 3=Max); **SF и BW должны совпасть** |
| Антенна | U.FL, диапазон 868 / 915 под регион | Аналогично |

**DIO2 и RF switch:** на **HT-RA62 / E22** часто DIO2 управляет переключателем RX/TX (`setDio2AsRfSwitch(true)`, см. `board.h` → `LORA_DIO2_RF_SWITCH`). У **RA-01SH** переключателя по DIO2 может не быть — тогда включение вредит приёму.

**TCXO vs кварц:** RA-01SH обычно на **кристалле**, не на TCXO. Режим TCXO в RadioLib при кварце даёт неверную настройку чипа и типичную картину «сосед нас слышит, мы его нет». Сборка под RA-01SH:

```bash
pio run -e faketec_v5_ble_ra01sh
```

Окружение `faketec_v5_ble_ra01sh` задаёт `-DLORA_MODULE_HAS_TCXO=0` и `-DLORA_DIO2_RF_SWITCH=0` (см. `board.h`, можно переопределять через `build_flags`).

**Serial:** ~30 с — `RX_CONT_ARM` (короткое окно RadioLib ~100 мс на вызов; не 30 с как у ESP). ~60 с — `HEARTBEAT … rx_tmo=… rx_pkt=… rx_err=…`: при **`rx_pkt=0`** до демодулятора кадры не доходят; при **`rx_pkt>0`** без соседей — смотрите `RX_PARSE_FAIL`.

**BLE и LoRa RX:** в ожидании кадра вызывается `ble::update()` через `radio::setRxBlePoll`, чтобы NUS не голодал при длинном ожидании DIO1.

### KEY_EXCHANGE: поиск причины (нет ключа / `hasKey: false`)

Критерий успеха при отладке: в UART — **`KEY_STORE_OK`** после приёма чужого публичного ключа; в `evt:neighbors` — **`hasKey: true`** для соседа (см. `x25519_keys::hasKeyFor` в `ble.cpp`).

1. **Два лога одновременно (1–2 мин после HELLO):** UART на faketec и на втором узле (Heltec). Свести таблицу: у кого **`KEY_TX ok=1`**, у кого появляется **`KEY_STORE_OK`** для пира. Если у A есть только TX, а у B нет **`KEY_STORE_OK`** — приём/парсинг на B.
2. **Передача не уходит:** смотреть **`KEY_TX ok=0`** (ошибка `radio::send`) или **`KEY_TX_SKIP`** с причиной в [`x25519_keys.cpp`](x25519_keys.cpp): `already_has_key`, `force_min_gap` (2500 ms после `hello_fwd`), `debounce`, `throttle`. Сопоставить время `HELLO_RX` и первого `KEY_TX`.
3. **Приём / формат:** успешное сохранение — **`KEY_STORE_OK`**. Если приходит KEY с неверной длиной полезной нагрузки, в лог пишется **`KEY_RX_BAD_LEN`** (ожидается 32 байта для X25519). При **`RX_PARSE_FAIL`** / **`len_mismatch`** — смотреть эфир и совпадение SF/BW/CR с соседом.
4. **Эфир:** та же таблица регион/модем, что выше; при `len_mismatch` на кадрах KEY не полагаться только на `KEY_TX ok=1`.
5. **«Завис»:** отдельно от ключа: **UART** ещё печатает (loop жив) или тишина; при **BLE** — очередь notify / нагрузка `evt:neighbors` (см. `ble.cpp`).

### RX_RECOVERY (после фикса «оглохший RX»)

Если эфир «застывает» (`rx_pkt` в `HEARTBEAT` не растёт, TX/HELLO ещё есть), в UART появится **`event=RX_RECOVERY`** в `faketec/radio.cpp`. В `main` — **`main_after_rx_recovery`** с тем же `seq`.

**Проверка по логам (3–5 мин рядом с соседом):**

1. После `RX_RECOVERY` снова появляются **`HELLO_RX`** / **`KEY_RX_RAW`** и при успехе **`KEY_STORE_OK`**.
2. В `HEARTBEAT` растёт **`rx_pkt`** (или хотя бы снова начинает расти после серии таймаутов).
3. Маркеры **`ALIVE`** / **`HEARTBEAT`** не пропадают — если пропали полностью, это уже не только RX (смотреть USB/питание/стек).

### Регрессия с Heltec (ручной чеклист, P0)

После изменений relay/dedup/radio прогнать в одной сети с узлом Heltec V3:

1. Pairing / KEY_EXCHANGE (появление ключа, `evt:neighbors`).
2. Unicast MSG в обе стороны.
3. Broadcast GROUP_MSG (если используете).
4. ACK после MSG с запросом ACK.

### Duty cycle (EU868 / лимиты регулятора)

Для регионов **EU** и **UK** в `radio::send` учитывается время в эфире по **RadioLib `getTimeOnAir`**: лимит **1% за час** (36 с), как в `firmware/src/duty_cycle/` на Heltec. Для **RU/US/AU** ограничение не применяется. При смене региона счётчик сбрасывается (`duty_cycle::reset()` из `region::setRegion`). Учитываются только **успешные** передачи.

## Ограничения (v1)

### Storage (FakeTech vs ESP NVS)

На **FakeTech** настройки и ключи лежат в **InternalFS** (flash) через модуль `storage`, плюс сессионные ключи X25519 в RAM — это **не** тот же бинарный формат, что **NVS** (`riftlink` namespace) на Heltec ESP32. **Перенос профиля между железками без ручного экспорта/импорта не предусмотрен:** совпадает протокол эфира и JSON BLE, а не снимок NVS.

### Планировщик vs ESP

Отдельная **FreeRTOS**-задача радио как в `async_tasks.cpp` на ESP **не дублируется:** роль очередей и deferred TX выполняет **`loop()` + `async_tx`**. При необходимости позже — вынести радио в задачу только по замерам задержек.

**SF в эфире из очереди:** как на Heltec V3/V4/Paper, TX из `queueTxPacket` / deferred использует **текущий mesh SF из настроек** (`radio::getMeshTxSfForQueue()`), а не адаптивные подсказки `rssiToSf` — паритет с `queueTxRequestInternal` в `firmware/src/async_tasks.cpp`. Иначе в логе был бы другой SF, чем в `evt:node` / пресете.

### Зависание: сценарий для отладки (faketec vs Heltec)

Чтобы отделить «протокол/ключи» от «подвис UI/BLE»:

1. **UART жив, BLE «мёртвый»** — `loop` и Serial идут, приложение не видит notify: типично **длинный LoRa TX** в том же потоке, что и SoftDevice (нет отдельной radio-task как на ESP), либо очередь NDJSON/NUS. Сборка **`faketec_v5`** (`-DRIFTLINK_SKIP_BLE`) изолирует mesh без BLE.
2. **Тишина и в UART** — смотреть стек/радио (`TX_RESULT`, `Duty cycle`), watchdog; не путать с отсутствием соседа на другом SF.
3. **Сопоставить длительность TX** в `RADIO event=TX_RESULT` с моментом пропадания BLE; после выравнивания SF с Heltec кадры короче при том же пресете — меньше блокировок в `loop()`.

- **InternalFS fallback:** при ошибке инициализации FS — откат на RAM для мелких ключей; крупные блобы (группы/offline) требуют успешного монтирования FS.
- **BLE:** Bluefruit52Lib `BLEUart` (Nordic UART UUID как в `docs/API.md`). Волна `node` → `neighbors` → `routes` → `groups` с `seq` и **`cmdId`**. **`evt:groups`** заполняется из локального **`groups::`** (после `groupCreate` и т.д.). Маршруты — из `routing`.
- **Стабильность BLE на nRF:** длинные NDJSON в NUS раньше шли с `delay(2)` на каждые ~20 B без `yield()` — при частых `evt:neighbors` после HELLO/KEY главный цикл мог долго не отдавать время SoftDevice (замирание UI/эфира). Сейчас между чанками — `yield()` + короче пауза; чтение длинной строки из NUS дробится с `yield()` каждые 48 символов; в `notifyInfo` между пачками событий добавлен `yield()`.
- **Mesh:** `ROUTE_REQ`/`ROUTE_REPLY`, `msg_queue`, фрагментация, batch/ACK coalesce, **network_coding** (`OP_XOR_RELAY`), `pkt_cache`/`OP_NACK`, relay-пайплайн с deferred relay и XOR — в объёме портов `*_nrf` и `main.cpp`. **OP_SF_BEACON** / **OP_NACK** в relay не участвуют (как на V3). **mab** / **beacon_sync** / **collision** / **clock_drift** — подключены через `v3_aux_nrf.cpp`; тонкая настройка таймингов эфира — после замеров HELLO-нагрузки, отдельными PR.
- **Шифрование:** ChaCha20-Poly1305 (libsodium), X25519 (`x25519_keys`). **Groups V2:** `ble.cpp` + `groups_storage`, Ed25519-подпись инвайта (`ed25519_sign/`, `group_owner_sign`); сценарии invite / accept / grant / revoke / rekey / snapshot — см. `ble.cpp` и паритет с `src/ble/ble.h`.
- **Wi‑Fi:** на nRF52840 **нет** модуля Wi‑Fi (в отличие от Heltec ESP32). Только **BLE** (телефон) и **LoRa** (mesh). В JSON для приложения: `radioMode: "ble"`, `wifiConnected: false`.
- **OTA:** только USB DFU (`firmware.zip`), не Wi‑Fi AP как на ESP.
- **Голос:** `voice_frag` — только `matchAck`/заглушки; чужие `OP_VOICE_*` ретранслируются при необходимости. **Location:** **нет GNSS** на плате; координаты приходят с телефона по BLE (`cmd:location` → тот же шифрованный broadcast `OP_LOCATION`, что на Heltec). Входящие `OP_LOCATION` с mesh → `evt:location` для **соседей** (relay). **Телеметрия:** `telemetry_nrf.cpp` — периодический `OP_TELEMETRY` (heap; батарея 0 без ADC).
- **Board:** `faketec_v5` + variant `faketec_nrf52840` (48 GPIO, см. `platformio.ini`)
