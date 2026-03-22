<p align="center">
  <img src="https://img.shields.io/badge/RiftLink-Protocol-42A5F5?style=for-the-badge&logo=radio&logoColor=white" alt="RiftLink" />
</p>

# 📡 RiftLink — спецификация протокола

> Формат пакетов, opcodes, шифрование, BLE, Serial

<p align="center">
  <img src="https://img.shields.io/badge/spec_version-0.2-888?style=flat-square" alt="Spec" />
  <img src="https://img.shields.io/badge/firmware-1.3.6-E7352C?style=flat-square&logo=espressif" alt="Firmware" />
  <img src="https://img.shields.io/badge/LoRa-SX1262-00B0FF?style=flat-square&logo=lorawan" alt="LoRa" />
  <img src="https://img.shields.io/badge/X25519-ChaCha20-4CAF50?style=flat-square" alt="Crypto" />
</p>

---

## 1. 📦 LoRa — формат пакета (v2)

**Формат v2** (version 0x20, с прошивки 1.2.6): Sync byte 0x5A, opcode первым, компактный broadcast (экономия 8 байт).

### 1.1 Broadcast (HELLO, PING, ROUTE_REQ и т.д.)

```
┌──────┬────────┬────────┬────────┬──────┬────────┬────────┐
│ Sync │ Ver    │ Opcode │ From   │ TTL  │ Channel│Payload │
│ 1 B  │ 1 B    │ 1 B    │ 8 B    │ 1 B  │ 1 B    │ N B    │
└──────┴────────┴────────┴────────┴──────┴────────┴────────┘
```
Заголовок: **13 байт** (Sync 0x5A + Ver + Opcode + From + TTL + Channel).

### 1.2 Unicast (MSG, ACK, KEY_EXCHANGE и т.д.)

```
┌──────┬────────┬────────┬────────┬────────┬──────┬────────┬────────┐
│ Sync │ Ver    │ Opcode │ From   │ To     │ TTL  │ Channel│Payload │
│ 1 B  │ 1 B    │ 1 B    │ 8 B    │ 8 B    │ 1 B  │ 1 B    │ N B    │
└──────┴────────┴────────┴────────┴────────┴──────┴────────┴────────┘
```
Заголовок: **21 байт**.

**Ver:** версия (4 bit) + флаги (encrypted, compressed, ack_req, broadcast).  
**From/To:** Node ID (8 байт). Broadcast = 0xFF…FF.

### 1.3 Payload

Следует после заголовка. Для MSG/LOCATION/TELEMETRY/GROUP_MSG/SOS используется ChaCha20-Poly1305 (encrypted payload).  
Для `GROUP_MSG`: `public` группы шифруются channel key, `private` группы — отдельным `groupKey` (32B) на каждый `groupId`.
Для private-group invite в приложении используется payload с `keyVersion` и коротким TTL (`inviteExpiryEpochSec`) для безопасной ротации.

### 1.4 Node ID и никнейм

- **Node ID:** 8 байт (16 hex), генерируется при первом запуске, хранится в NVS. Broadcast = все 0xFF.
- **Legacy short-id (8 hex) не поддерживается** в API/CLI и клиентских командах.
- **Никнейм:** до 16 символов, опционально, хранится в NVS. Отображается в приложении вместо/вместе с ID.

### 1.5 Channel / Lane

- `channel=0` — normal lane (`CHANNEL_DEFAULT`)
- `channel=1` — critical lane (`CHANNEL_CRITICAL`)

Critical lane используется для SOS/приоритетных сообщений и обрабатывается отдельной политикой ретрансляции.
Для SOS используется адаптивный TTL (по плотности соседей) и лимит relay как глобально, так и per-source.

### 1.6 Groups V2 (thin-device, no-legacy)

Groups V2 использует `groupUID` как основной идентификатор группы. Транспортный `group_id` допускается только как короткий radio-routing идентификатор (`channelId32`) и не является правом доступа.

Минимальная модель V2:

- `groupUID` — неизменяемый ID группы (не секрет).
- `channelId32` — короткий transport ID для эфира (`>= 2` для пользовательских групп; `1` зарезервирован под `GROUP_ALL`).
- `groupTag` — дополнительный контекстный маркер группы.
- `canonicalName` — каноническое имя группы для UI (управляется owner).
- `ownerSignPubKey` — owner Ed25519 public key для signed invite.
- `groupSecret` (32B) + `groupKeyVersion`.
- Роли: `owner`, `admin`, `member`.

Право доступа определяется не ID группы, а валидным role-grant и актуальной версией ключа.
При обработке `GROUP_MSG` приложение использует `channelId32` для маршрутизации в чат и, при наличии, `groupUID` для канонического сопоставления с хранилищем.

#### 1.6.1 Root of trust

- Единственный корень доверия: публичный ключ owner.
- Роли `admin/member` подтверждаются signed grant от owner.
- Для admin-операций требуется:
  - валидный owner-signed grant;
  - actor proof отправителя (кто инициировал команду);
  - отсутствие replay и устаревшей revocation-версии.

#### 1.6.2 Thin-device storage

Для экономии NVS устройство хранит только runtime security state:

- `groupUID/channelId32/groupTag/canonicalName`;
- pinned `ownerSignPubKey` (TOFU при первом `groupInviteAccept`), далее строгая проверка совпадения.
- текущий `groupSecret` и `groupKeyVersion`;
- локальная роль узла (`myRole`);
- `revocationEpoch` (watermark отзывов);
- компактный anti-replay state.

Полная история `grants/revokes/rekey per-member` хранится на телефоне и синхронизируется на устройство snapshot-пакетами.

#### 1.6.3 Rekey and kick policy

- `kick` всегда: `revoke` + mandatory `rekey`.
- Новый ключ считается примененным участником только после `ACK_KEY_APPLIED`.
- Fail-closed: если контекст группы/роль/версия/replay-проверка не проходит, пакет отклоняется.

---

## 2. 📋 Opcodes

| Код | Имя | Описание |
|-----|-----|----------|
| 0x01 | MSG | Текстовое сообщение (шифр.) |
| 0x02 | ACK | Подтверждение получения (msg_id 4 байта) |
| 0x03 | HELLO | Beacon соседей (без payload) |
| 0x04 | ROUTE_REQ | Запрос маршрута (broadcast, payload: target[8], req_id[4], hops[1], sender[8]) |
| 0x05 | ROUTE_REPLY | Ответ маршрута (unicast, payload: target[8], req_id[4], hops[1], originator[8]) |
| 0x06 | KEY_EXCHANGE | X25519: публичный ключ 32 байта (unicast) |
| 0x07 | TELEMETRY | Батарея + heap (шифр., broadcast) |
| 0x08 | LOCATION | Геолокация lat/lon/alt (шифр., broadcast) |
| 0x09 | GROUP_MSG | Групповое сообщение (шифр., broadcast, payload: [channelId32 4B][text]) |
| 0x0A | MSG_FRAG | Фрагмент длинного сообщения |
| 0x0B | VOICE_MSG | Голосовое сообщение (Opus, фрагменты как MSG_FRAG) |
| 0x0C | READ | Подтверждение прочтения (payload: msg_id 4B) |
| 0x0D | NACK | Запрос повтора (payload: pktId 2B, v2.1) |
| 0x0E | ECHO | Эхо вместо ACK: broadcast msgId+originalFrom (12B) |
| 0x0F | POLL | RIT: broadcast «присылайте пакеты для меня» (payload пустой) |
| 0x10 | MSG_BATCH | Packet Fusion: count(1) + [len(2)+enc]* — несколько MSG в одном пакете |
| 0x11 | XOR_RELAY | Network Coding: XOR(A,B) broadcast, meta: pktIdA/B, fromA/B, toA/B |
| 0x12 | SF_BEACON | Broadcast beacon с текущим mesh SF |
| 0x13 | ACK_BATCH | Батч ACK: count(1) + msgId(4)* |
| 0x14 | SOS | Критический emergency flood (encrypted broadcast) |
| 0xFE | PONG | Ответ на PING |
| 0xFF | PING | Проверка связи (получатель отвечает PONG) |

---

## 3. 🔐 Шифрование

- **Алгоритм:** ChaCha20-Poly1305 (libsodium)
- **Unicast (E2E):** X25519 per-peer. KEY_EXCHANGE (32 байта pub_key) при HELLO. Shared secret = crypto_box_beforenm.
- **Broadcast:** общий ключ 32 байта в NVS.
- **Важно:** unicast `encryptFor()` требует pairwise-ключ; fallback на channel key для DM не используется.
- При принятом invite публичный ключ пира считается pinned; при последующем `KEY_EXCHANGE` с другим pubKey устройство отправляет `evt:error` (`code:"invite"`).
- При принятом invite публичный ключ пира считается pinned; при последующем `KEY_EXCHANGE` с другим pubKey устройство отправляет `evt:error` (`code:"invite_peer_key_mismatch"`).
- **Nonce:** 12 байт (случайный на пакет)
- **Overhead:** 28 байт (nonce + tag)

Порядок: Plaintext → [LZ4 при ≥50 байт] → ChaCha20-Poly1305 → LoRa

---

## 4. 🗜️ Сжатие (LZ4)

- Применяется к тексту **≥50 байт** до шифрования
- Флаг `compressed` в заголовке
- Получатель: расшифровка → распаковка (если флаг)

---

## 5. 📦 Фрагментация (MSG_FRAG)

Для сообщений >200 байт после шифрования:

| Поле | Размер | Описание |
|------|--------|----------|
| MsgID | 4 B | ID сообщения |
| Part | 1 B | Номер фрагмента (1..Total) |
| Total | 1 B | Всего фрагментов |
| Data | ≤194 B | Часть зашифрованных данных |

Макс. plaintext: 2048 байт. Макс. фрагментов: 32.

---

## 6. ✓ ACK, READ и retransmit

- Unicast MSG с `ack_req`: первые 4 байта plaintext = msg_id
- Получатель отправляет ACK с этим msg_id (✓✓ доставлено)
- Получатель отправляет READ (OP_READ, payload: msg_id) при просмотре сообщения (✓✓✓ прочитано)
- Отправитель повторяет до 4 раз при отсутствии ACK (для `critical` до 6 попыток)
- ACK timeout зависит от SF: `SF7≈2.5s … SF12≈5s` (`getAckTimeoutMs`), не фиксированные 6 секунд
- Очередь: до 12 pending unicast, до 4 broadcast; после неудачи → offline_queue (до 16 в NVS)

---

## 7. 📱 BLE — JSON протокол

**Сервис:** `6e400001-b5a3-f393-e0a9-e50e24dcca9e`  
**TX (write):** `6e400002-...`  
**RX (notify):** `6e400003-...`

**Фрагментация по MTU:** несколько JSON подряд без разделителя приложение склеивает по скобкам; прошивка после **каждого** JSON в notify добавляет `\n` (NDJSON: один JSON на строку; в `ble.cpp` — `notifyJsonToApp`). Если JSON ровно 512 байт, `\n` не добавляется (редкий край). Внутри строковых полей неэкранированный `\n` недопустим.

**Важное ограничение BLE transport:** запись команд из app выполняется как `write without response`, поэтому подтверждение на этом уровне не гарантируется; прикладной статус доставки задаётся событиями `sent/delivered/read/undelivered`.

### 7.1 Команды (приложение → устройство)

| cmd | Параметры | Описание |
|-----|-----------|----------|
| send | text, to?, lane?, trigger?, triggerAtMs? | Отправить сообщение (normal/critical, Time Capsule trigger) |
| sos | text? | Emergency flood |
| location | lat, lon, alt, radiusM?, expiryEpochSec? | Геолокация (в т.ч. geofence) |
| read | from, msgId | Маркер «прочитано» для входящего сообщения |
| info | — | Запросить `evt:"info"` snapshot |
| groups | — | Запросить `evt:"groups"` snapshot |
| neighbors | — | Запросить `evt:"neighbors"` snapshot |
| ping | to? | Диагностический ping (`evt:"pong"`) |
| voice | to, chunk, total, data(base64) | Отправка голосовых фрагментов |

GeoFence baseline hardening (приёмник):
- валидный диапазон координат (`lat/lon`);
- ограничение `radiusM <= 50000`;
- фильтрация явно некорректного `expiryEpochSec`;
- anti-spoof по нереалистичному скачку позиции относительно предыдущего пакета от того же узла.
| ota | — | Запустить OTA (WiFi AP) |
| region | region | Установить регион (EU, UK, RU, US, AU) |
| routes | — | Запросить маршруты (evt "routes") |

Список команд выше — рабочий минимум для app↔device, не исчерпывающий для всех debug/maintenance команд прошивки.

### 7.2 События (устройство → приложение)

| evt | Поля | Описание |
|-----|------|----------|
| info | id, nickname?, region, freq, power, channel?, neighbors?, version? | При подключении |
| neighbors | neighbors | Обновление списка соседей (при новом HELLO) |
| routes | routes | Маршруты: [{dest, nextHop, hops, rssi, trustScore}] |
| msg | from, text, lane?, type?, group?, groupUid? | Входящее сообщение (normal/critical, text/sos/...); для `GROUP_MSG` добавляются `group=channelId32` и опционально `groupUid` |
| sent | to, msgId | Сообщение принято в обработку на узле (не ACK mesh-доставки) |
| delivered | from, msgId, rssi? | Получен ACK доставки от адресата |
| read | from, msgId, rssi? | Получен READ от адресата |
| undelivered | to, msgId, delivered?, total? | Доставка не подтверждена/частично подтверждена |
| broadcast_delivery | msgId, delivered, total | Итог по broadcast доставке |
| location | from, lat, lon, alt | Геолокация от узла |
| telemetry | from, battery, heapKb | Телеметрия |
| relayProof | relayedBy, from, to, pktId, opcode | Proof-of-Relay Lite (sampling) |
| timeCapsuleQueued | to, trigger, triggerAtMs? | Подтверждение постановки в отложенную отправку |
| timeCapsuleReleased | to, msgId, trigger | Сработал trigger отложенной отправки |
| ota | ip, ssid, password | OTA запущен |
| region | region, freq, power | Регион изменён |

---

## 7.1 🛣️ ROUTE_REQ / ROUTE_REPLY (проактивный маршрутинг)

AODV-подобный поиск маршрута:

- **ROUTE_REQ** (broadcast): target[8], req_id[4], hops[1], sender[8]. Цель отвечает ROUTE_REPLY.
- **ROUTE_REPLY** (unicast): target[8], req_id[4], hops[1], originator[8]. Пересылается по обратному пути.
- Таблица маршрутов: до 16 записей, TTL 2 мин.
- Serial: `route <hex16>` — запрос маршрута до узла.

---

## 7.2 🎤 VOICE_MSG (голосовые сообщения)

- **Формат:** как MSG_FRAG (msgId, part, total, data). Payload — зашифрованный Opus.
- **Кодек:** Opus 8 kbps на телефоне.
- **Лимит:** ~30 KB (30 сек).
- **BLE:** cmd "voice" — чанки base64; evt "voice" — чанки на приём.
- **Mesh-adaptive профиль (baseline):** `fast / balanced / resilient` (приложение выбирает по RSSI/SF/числу соседей, прошивка адаптирует SF/channel/пейсинг фрагментов).
- **RX/playback strategy (app baseline+):** при неполной сборке допускается salvage непрерывного префикса (chunk 0..N без дыр) с явной маркировкой packet-loss; при воспроизведении рекомендуется fallback попытка по альтернативному codec-path (Opus/AAC).
- **Acceptance baseline (app debug):** quality verdict `PASS/WARN/FAIL/WARMUP` по метрикам `avgLoss/hardLoss/minSessions`; пороги задаются в Settings и фиксируются в debug snapshot.

---

## 8. ⌨️ Serial команды

| Команда | Описание |
|---------|----------|
| send &lt;text&gt; | Broadcast сообщение |
| send &lt;hex16&gt; &lt;text&gt; | Unicast на узел (hex16 = 16 hex-символов полного ID) |
| ping &lt;hex16&gt; | Проверка связи (отправить PING, ждать PONG) |
| route &lt;hex16&gt; | Запросить маршрут до узла (ROUTE_REQ) |
| region EU\|UK\|RU\|US\|AU | Установить регион |

---

## 9. 🌍 Регионы

| Код | Частота | Мощность | Duty cycle |
|-----|---------|----------|------------|
| EU | 868.1 / 868.3 / 868.5 MHz (3 канала LoRaWAN) | 14 dBm | 1% (36 с/час) |
| UK | как EU | 14 dBm | 1% (36 с/час) |
| RU | 868.8 MHz | 20 dBm | — |
| US | 915.0 MHz | 22 dBm | — |
| AU | 915.0 MHz | 22 dBm | — |

**EU/UK:** выбор из 3 LoRaWAN-совместимых каналов (ETSI G1). Канал 0 = 868.1, 1 = 868.3, 2 = 868.5 MHz.

---

## 10. 📻 Радио

- **Модуль:** SX1262 (RadioLib)
- **Параметры:** SF7, BW 125 kHz, CR 5
- **LoRa Sync word:** 0x12 (приватная сеть)
- **Sync byte пакета:** 0x5A (маркер начала, поиск при сдвиге при RF-коррупции)
- **CRC:** 2 байта
