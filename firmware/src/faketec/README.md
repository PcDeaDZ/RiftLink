# RiftLink для FakeTech V5 Rev B

Прошивка RiftLink для платы **FakeTech V5 Rev B** (NiceNano nRF52840 + HT-RA62/RA-01SH LoRa).

> **Минимальная ветка (актуально для сборки):** `pio run -e faketec_v5` и `-e heltec_t114` проходят. LoRa — `radio_nrf.cpp` (SX1262, TCXO, DIO2 RF switch, KV для модема), точка входа — `faketec/main.cpp`. BLE — NUS в `ble_nrf.cpp` (Bluefruit); очередь исходящих mesh — общий `msg_queue/msg_queue.cpp` + `packet_fusion` + `mab` (BLS-N/ESP-NOW отключены флагами `-DRIFTLINK_DISABLE_BLS_N` / `DISABLE_ESP_NOW`), TX через `async_stubs_nrf.cpp` → `radio::sendDirectInternal`; колбэки BLE — `main.cpp` (`ble::setOnSend`, `notifySent` / `undelivered`). **Mesh HELLO/POLL** и тихое окно после handshake — `mesh_hello_nrf.cpp` / `mesh_hello_nrf.h`, вызов из `loop` в `faketec/main.cpp`. **OLED I2C:** `display_nrf` + Adafruit SSD1306/GFX в `lib_deps`; пины `Wire.setPins` из `board_pins.h`; в `variants/faketec_nrf52840/variant.h` задано `WIRE_INTERFACES_COUNT=1` (без второго TWIM, иначе ядро требует `PIN_WIRE1_*`). **Heltec T114:** встроенный **ST7789** (SPI1) в `display_nrf.cpp` при `RIFTLINK_BOARD_HELTEC_T114`, пины TFT/подсветка — `board_pins.h` (сверено с upstream Meshtastic `heltec_mesh_node_t114`). Ключи X25519: `crypto_box_keypair` заменён на `randombytes_buf` + `crypto_scalarmult_base` + HSalsa (урезанный esphome/libsodium без `crypto_box`). Прошивка с ПК: см. `docs/flasher/NRF52.md`; из корня репозитория: `.\build.ps1 -Faketec` / `-HeltecT114`.

### Что ещё не сделано (кратко)

- **UI / меню как на Heltec V3** — нет `ui/display.cpp`, вкладок, кнопки и long press; на nRF только минимальная отрисовка: I2C OLED (FakeTech) или текст на ST7789 (T114). Полный порт UI — отдельный эпик (см. раздел «Дисплей и меню»).
- **BLE OTA** (`bleOtaStart` / chunk / …) — не реализовано; обновление: DFU (NiceNano) или `nrfutil` (см. `docs/flasher/NRF52.md`). В BLE приходит `evt:error` с кодом `ble_ota_unsupported`.
- **Голос по BLE** (`cmd: voice` → mesh `OP_VOICE_MSG`, `evt: voice` с эфира) — реализовано в `ble_nrf.cpp` (паритет с ESP).
- **Wi‑Fi, ESP‑NOW** (`wifi`, `espnowChannel`, `espnowAdaptive`) — не применимо; явные ошибки в BLE. Команда **`ota`** (AP для OTA как в §2.3 API) — `ota_unsupported`; обновление только DFU/USB.
- **Poweroff / powersave по BLE** — не реализованы; коды `poweroff_unsupported`, `powersave_unsupported`.

### Архитектура (паритет с Heltec V3, без Wi‑Fi)

- **Поток:** один **`loop()`** в `faketec/main.cpp`: `ble::update`, `msg_queue`/`routing`/`offline_queue`, **`flushDeferredSends()`** (`async_stubs_nrf.cpp`: `queueTxPacket` → `radio::sendDirectInternal`, отложенный TX, `ack_coalesce::flush`), затем **`mesh_hello_nrf_loop()`** (HELLO/POLL, тихое окно handshake), далее **периодический KEY retry** для соседей без сессионного ключа (интервалы как в ESP `main.cpp`, причина `"retry"`) и раз в 60 с **`MODEM_SNAPSHOT`** в `RIFTLINK_DIAG`. USB Serial: **`neighbors`** / **`nb`**, **`node`** / **`id`**, **`version`** / **`ver`**, **`uptime`**, **`sf`** (с bw/cr), **`espnow`** (заглушка) — полный перечень в `docs/API.md` §4. На **T114** нажатие кнопки дублирует подсказку на ST7789 (`queue_last_msg`). Приём LoRa — **`radio::receive`** под mutex (`radio_nrf.cpp`), разбор — **`handlePacket`** (`handle_packet_nrf.cpp` + `handle_packet_nrf_body.inc`), паритет с ESP по relay/XOR/очередям.
- **Хранилище:** InternalFS через **`kv.cpp`** (`riftlink_kv::` — блобы в `/rl_*`). При ошибке монтирования LittleFS вызывается **`InternalFS.format()`** и повторный `begin()` (как в Meshtastic `FSBegin` для nRF52). При полной порче раздела — factory erase UF2 (см. `docs/flasher/NRF52.md`). Перед записью ключа файл удаляется, чтобы не копить append на nRF. **Группы V2** и **offline_queue** — общие `groups/groups.cpp` и `offline_queue/offline_queue.cpp` с веткой **`RIFTLINK_NRF52`** (KV вместо NVS).
- **Сборка `nrf52_base` (`platformio.ini`):** подключаются те же модули, что и на ESP для mesh: `msg_queue`, `packet_fusion`, `mab`, `frag`, `routing`, `ack_coalesce`, `groups`, `offline_queue`, `beacon_sync`, `clock_drift`, `voice_frag`, `voice_buffers`, плюс порты в `faketec/*` (`gps_nrf.cpp` вместо `gps/gps.cpp`). Файла **`nrf_mesh_stubs.cpp`** больше нет — заглушки выведены.
- **GPS:** **`gps_nrf.cpp`** — без модуля GNSS; время/координаты для гео и epoch — через **`setPhoneSync`** (BLE), пины/флаг питания в KV.
- **Голос:** общий **`voice_frag`/`voice_buffers`** (на nRF память через `malloc` вместо `heap_caps`).

## Дисплей и меню

**I2C OLED SSD1306:** `faketec/display_nrf.*` + `Adafruit SSD1306` / `GFX` в `nrf52_base` (`lib_deps`). Пины — `board_pins.h` (`PIN_I2C_SDA`/`SCL`). Бут-строка и самотест на экране; последнее сообщение mesh — через `queueDisplayLastMsg` → `display_nrf::poll()` в `loop`.

**ST7789 (Heltec Mesh Node T114):** при сборке **`heltec_t114`** (`RIFTLINK_BOARD_HELTEC_T114`) используется ветка в `display_nrf.cpp`: Adafruit ST7789 на **SPI1**, пины CS/DC/RST/SCK/MOSI и подсветка — в `board_pins.h` (ориентир — upstream Meshtastic `variants/nrf52840/heltec_mesh_node_t114`). Отрисовка упрощённая (текст, без полного UI ESP).

**План расширения (отдельный эпик):** полноценное меню/`queueDisplayRedraw` как на V3, вкладки, кнопка — не в `nrf52_base` `build_src_filter`; при необходимости — урезанное меню или только доработка текстового слоя на ST7789. Не тянуть TinyUSB/SdFat без нужды.

Общие модули рефакторинга UI (`firmware/src/ui/ui_scroll.h`, `ui_menu_exec.*`, `ui_layout_profile.h`, `ui_icons.h` и т.д.) подключаются только в ESP-сборках (`heltec_*`, `lilygo_*`); env **faketec_v5** собирает `faketec/` + `protocol/` и не тянет эти файлы.

## Длинные сообщения

Текст длиннее одного LoRa-пакета уходит в **`frag::send`** (`frag/frag.cpp` собирается в `faketec_v5`). Если **`frag::send`** вернёт `false` (нет ключа, переполнение слотов), приложение получит **`evt:error`** с кодом **`send_too_long`** (`msg_queue::SEND_FAIL_FRAG_UNAVAILABLE`).

## Сборка

```bash
cd firmware
pio run -e faketec_v5
```

- **`faketec_v5`** — единственная обычная сборка: **BLE (NUS)** + LoRa mesh, `FAKETEC_RADIO_TASK=1` по умолчанию. Отладка без лишних env: см. комментарии в `platformio.ini` (`FAKETEC_RX_VERBOSE`, RA-01SH, отключение radio-task).

Артефакты: `.pio/build/<env>/firmware.hex`, `firmware.zip`

## Прошивка

Кратко: **`docs/flasher/NRF52.md`** (nrfutil, DFU, отличие от ESP Web Flash).

1. **DFU-режим:** двойной клик по кнопке RST на NiceNano (в течение 0.5 с)
2. Появится диск `NICENANO` — скопируйте `firmware.zip` из `.pio/build/faketec_v5/` (или `firmware/out/faketec_v5/` после `.\build.ps1 -Faketec`) на диск (или используйте `nrfutil`)

Или через PlatformIO:
```bash
pio run -t upload -e faketec_v5
```

Из корня репозитория (сборка и копирование артефактов в `firmware/out/`):
```powershell
.\build.ps1 -Faketec
.\build.ps1 -Faketec -Flash -Port COM5
```

## Распиновка (board.h) и выбор `pio -e`

| Плата | `pio run -e …` | Примечание |
|-------|----------------|------------|
| FakeTech V5 / NiceNano / Pro Micro DIY (как ниже) | **`faketec_v5`** | Таблица пинов — эта секция |
| Heltec Mesh Node **T114** | **`heltec_t114`** | Другие I2C/SPI в `board_pins.h` (OLED: 26/27); не прошивать `faketec_v5` на T114 и наоборот |

Плата `heltec_mesh_t114.json` в репозитории использует **тот же** вариант `faketec_nrf52840`, что и FakeTech; различие только **макрос** `RIFTLINK_BOARD_HELTEC_T114` и строки в `board_pins.h`.

### FakeTech / Pro Micro (env `faketec_v5`)

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
- **Команды:** BLE GATT (UUID как в `docs/API.md`) и USB Serial — JSON, до 512 байт на запись. В USB Serial для отладки — те же сценарии, что и в перечне **§4 `docs/API.md`** (включая **`node`**, **`espnow`**, **`powersave`**, **`version`**, **`uptime`**, расширенный **`sf`**). Двухузловой ручной чеклист: [`docs/NRF52_PARITY_CHECKLIST.md`](../../../docs/NRF52_PARITY_CHECKLIST.md).
- **Совместимость:** тот же протокол, что и Heltec — узлы в одной сети

## Совместимость с Heltec V3 (приём / «вижу только в одну сторону»)

Параметры эфира должны совпадать с соседом, иначе декодирования не будет.

| Проверить | FakeTech | Heltec V3 |
|-----------|----------|-----------|
| Регион / частота | `evt:node` → `region`, `freq`, при EU ещё `channel` (0/1/2) | То же в приложении / NVS |
| Modem | `sf`, `bw`, `cr`, `modemPreset` в `evt:node` | Тот же пресет (0=Speed … 3=Max); **SF и BW должны совпасть** |
| Антенна | U.FL, диапазон 868 / 915 под регион | Аналогично |

**DIO2 и RF switch:** на **HT-RA62 / E22** часто DIO2 управляет переключателем RX/TX (`setDio2AsRfSwitch(true)`, см. `board.h` → `LORA_DIO2_RF_SWITCH`). У **RA-01SH** переключателя по DIO2 может не быть — тогда включение вредит приёму.

**TCXO vs кварц:** RA-01SH обычно на **кристалле**, не на TCXO. Режим TCXO в RadioLib при кварце даёт неверную настройку чипа и типичную картину «сосед нас слышит, мы его нет». Для RA-01SH добавьте в **`build_flags`** секции `faketec_v5` в `platformio.ini`: `-DLORA_MODULE_HAS_TCXO=0` и `-DLORA_DIO2_RF_SWITCH=0` (см. `board.h`).

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

На **FakeTech** настройки и ключи лежат в **InternalFS** через **`riftlink_kv`** (`faketec/kv.cpp`: файлы `/rl_<key>`), плюс сессионные ключи X25519 в RAM — это **не** тот же бинарный формат, что **NVS** (`riftlink` namespace) на Heltec ESP32. **Перенос профиля между железками без ручного экспорта/импорта не предусмотрен:** совпадает протокол эфира и JSON BLE, а не снимок NVS.

### Планировщик vs ESP

Отдельная **FreeRTOS**-задача радио как в `async_tasks.cpp` на ESP **не дублируется:** роль очередей и deferred TX выполняет **`loop()` + `async_tx`**. При необходимости позже — вынести радио в задачу только по замерам задержек.

**SF в эфире из очереди:** как на Heltec V3/V4/Paper, TX из `queueTxPacket` / deferred использует **текущий mesh SF из настроек** (`radio::getMeshTxSfForQueue()`), а не адаптивные подсказки `rssiToSf` — паритет с `queueTxRequestInternal` в `firmware/src/async_tasks.cpp`. Иначе в логе был бы другой SF, чем в `evt:node` / пресете.

### Зависание: сценарий для отладки (faketec vs Heltec)

Чтобы отделить «протокол/ключи» от «подвис UI/BLE»:

1. **UART жив, BLE «мёртвый»** — `loop` и Serial идут, приложение не видит notify: типично **длинный LoRa TX** или перегрузка NUS; на nRF приём LoRa вынесен в `radio_task`, BLE остаётся в `loop`. Для проверки только mesh без BLE временно добавьте в `build_flags`: `-DRIFTLINK_SKIP_BLE`.
2. **Тишина и в UART** — смотреть стек/радио (`TX_RESULT`, `Duty cycle`), watchdog; не путать с отсутствием соседа на другом SF.
3. **Сопоставить длительность TX** в `RADIO event=TX_RESULT` с моментом пропадания BLE; после выравнивания SF с Heltec кадры короче при том же пресете — меньше блокировок в `loop()`.

- **InternalFS fallback:** при ошибке инициализации FS — откат на RAM для мелких ключей; крупные блобы (группы/offline) требуют успешного монтирования FS.
- **BLE:** Bluefruit52Lib `BLEUart` (Nordic UART UUID как в `docs/API.md`). Волна `node` → `neighbors` → `routes` → `groups` с `seq` и **`cmdId`**. **`evt:groups`** заполняется из локального **`groups::`** (после `groupCreate` и т.д.). Маршруты — из `routing`.
- **Стабильность BLE на nRF:** длинные NDJSON в NUS раньше шли с `delay(2)` на каждые ~20 B без `yield()` — при частых `evt:neighbors` после HELLO/KEY главный цикл мог долго не отдавать время SoftDevice (замирание UI/эфира). Сейчас между чанками — `yield()` + короче пауза; чтение длинной строки из NUS дробится с `yield()` каждые 48 символов; в `notifyInfo` между пачками событий добавлен `yield()`.
- **Mesh:** `ROUTE_REQ`/`ROUTE_REPLY`, `msg_queue`, фрагментация, batch/ACK coalesce, **network_coding** (`OP_XOR_RELAY`), `pkt_cache`/`OP_NACK`, relay-пайплайн с deferred relay и XOR — в `handle_packet_nrf_body.inc` / общих модулях. **OP_SF_BEACON** / **OP_NACK** в relay не участвуют (как на V3). **mab**, **beacon_sync**, **collision_slots**, **clock_drift** — общие `.cpp` в `nrf52_base`.
- **Шифрование:** ChaCha20-Poly1305 (libsodium), X25519 (`x25519_keys`). **Groups V2:** `ble.cpp` + `groups_storage`, Ed25519-подпись инвайта (`ed25519_sign/`, `group_owner_sign`); сценарии invite / accept / grant / revoke / rekey / snapshot — см. `ble.cpp` и паритет с `src/ble/ble.h`.
- **Wi‑Fi:** на nRF52840 **нет** модуля Wi‑Fi (в отличие от Heltec ESP32). Только **BLE** (телефон) и **LoRa** (mesh). В JSON для приложения: `radioMode: "ble"`, `wifiConnected: false`.
- **OTA:** только USB DFU (`firmware.zip`), не Wi‑Fi AP как на ESP.
- **Голос:** общий **`voice_frag` / `voice_buffers`** (приём/сборка фрагментов, шифрование). **Location / GPS:** **`gps_nrf.cpp`** — без GNSS; координаты и epoch с телефона (**`gps::setPhoneSync`** по BLE). Входящие `OP_LOCATION` с mesh → `evt:location` (relay). Периодический **`OP_TELEMETRY`** (broadcast, как на ESP) — **`telemetry_nrf.cpp`**, интервал в **`main.cpp`** (`TELEM_INTERVAL_MS`). На **T114** напряжение батареи в телеметрии берётся с АЦП (делитель как в Meshtastic: `T114_BATT_ADC_PIN` / `T114_ADC_CTRL_PIN` в `board_pins.h`); на **FakeTech** без схемы делителя в прошивке поле батареи может оставаться 0.
- **Board:** `faketec_v5` + variant `faketec_nrf52840` (48 GPIO, см. `platformio.ini`)
