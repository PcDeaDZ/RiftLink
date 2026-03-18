# RiftLink — спецификация протокола

Версия: 0.1 (по состоянию реализации)

---

## 1. LoRa — формат пакета

### 1.1 Заголовок (20 байт)

| Байт | Поле | Описание |
|------|------|----------|
| 0 | version_flags | Версия (4 бита) + флаги: encrypted=0x08, compressed=0x04, ack_req=0x02 |
| 1–8 | from | Node ID отправителя (8 байт) |
| 9–16 | to | Node ID получателя (8 байт), 0xFF…FF = broadcast |
| 17 | ttl | Time-to-live для роутинга (31 макс.) |
| 18 | opcode | Код операции |
| 19 | channel | Логический канал (0 = публичный) |

### 1.2 Payload

Следует после заголовка. Для MSG — зашифрованный текст (ChaCha20-Poly1305). Для broadcast — без шифрования (опционально).

### 1.3 Node ID и никнейм

- **Node ID:** 8 байт, генерируется при первом запуске, хранится в NVS. Broadcast = все 0xFF.
- **Никнейм:** до 16 символов, опционально, хранится в NVS. Отображается в приложении вместо/вместе с ID.

---

## 2. Opcodes

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
| 0x09 | GROUP_MSG | Групповое сообщение (шифр., broadcast, payload: [group_id 4B][text]) |
| 0x0A | MSG_FRAG | Фрагмент длинного сообщения |
| 0x0B | VOICE_MSG | Голосовое сообщение (Opus, фрагменты как MSG_FRAG) |
| 0x0C | READ | Подтверждение прочтения (payload: msg_id 4B) |
| 0xFE | PONG | Ответ на PING |
| 0xFF | PING | Проверка связи (получатель отвечает PONG) |

---

## 3. Шифрование

- **Алгоритм:** ChaCha20-Poly1305 (libsodium)
- **Unicast (E2E):** X25519 per-peer. KEY_EXCHANGE (32 байта pub_key) при HELLO. Shared secret = crypto_box_beforenm.
- **Broadcast:** общий ключ 32 байта в NVS (fallback для unicast до обмена ключами)
- **Nonce:** 12 байт (случайный на пакет)
- **Overhead:** 28 байт (nonce + tag)

Порядок: Plaintext → [LZ4 при ≥50 байт] → ChaCha20-Poly1305 → LoRa

---

## 4. Сжатие (LZ4)

- Применяется к тексту **≥50 байт** до шифрования
- Флаг `compressed` в заголовке
- Получатель: расшифровка → распаковка (если флаг)

---

## 5. Фрагментация (MSG_FRAG)

Для сообщений >200 байт после шифрования:

| Поле | Размер | Описание |
|------|--------|----------|
| MsgID | 4 B | ID сообщения |
| Part | 1 B | Номер фрагмента (1..Total) |
| Total | 1 B | Всего фрагментов |
| Data | ≤194 B | Часть зашифрованных данных |

Макс. plaintext: 2048 байт. Макс. фрагментов: 32.

---

## 6. ACK, READ и retransmit

- Unicast MSG с `ack_req`: первые 4 байта plaintext = msg_id
- Получатель отправляет ACK с этим msg_id (✓✓ доставлено)
- Получатель отправляет READ (OP_READ, payload: msg_id) при просмотре сообщения (✓✓✓ прочитано)
- Отправитель повторяет до 3 раз при отсутствии ACK (таймаут 6 с)
- Очередь: до 8 pending сообщений

---

## 7. BLE — JSON протокол

**Сервис:** `6e400001-b5a3-f393-e0a9-e50e24dcca9e`  
**TX (write):** `6e400002-...`  
**RX (notify):** `6e400003-...`

### 7.1 Команды (приложение → устройство)

| cmd | Параметры | Описание |
|-----|-----------|----------|
| send | text, to? | Отправить сообщение (to = hex8 для unicast) |
| location | lat, lon, alt | Отправить геолокацию (broadcast) |
| ota | — | Запустить OTA (WiFi AP) |
| region | region | Установить регион (EU, RU, US, AU) |
| routes | — | Запросить маршруты (evt "routes") |

### 7.2 События (устройство → приложение)

| evt | Поля | Описание |
|-----|------|----------|
| info | id, nickname?, region, freq, power, channel?, neighbors? | При подключении |
| neighbors | neighbors | Обновление списка соседей (при новом HELLO) |
| routes | routes | Маршруты: [{dest, nextHop, hops, rssi}] для mesh-визуализации |
| msg | from, text | Входящее сообщение |
| location | from, lat, lon, alt | Геолокация от узла |
| telemetry | from, battery, heapKb | Телеметрия |
| ota | ip, ssid, password | OTA запущен |
| region | region, freq, power | Регион изменён |

---

## 7.1 ROUTE_REQ / ROUTE_REPLY (проактивный маршрутинг)

AODV-подобный поиск маршрута:

- **ROUTE_REQ** (broadcast): target[8], req_id[4], hops[1], sender[8]. Цель отвечает ROUTE_REPLY.
- **ROUTE_REPLY** (unicast): target[8], req_id[4], hops[1], originator[8]. Пересылается по обратному пути.
- Таблица маршрутов: до 16 записей, TTL 2 мин.
- Serial: `route <hex8>` — запрос маршрута до узла.

---

## 7.2 VOICE_MSG (голосовые сообщения)

- **Формат:** как MSG_FRAG (msgId, part, total, data). Payload — зашифрованный Opus.
- **Кодек:** Opus 8 kbps на телефоне.
- **Лимит:** ~30 KB (30 сек).
- **BLE:** cmd "voice" — чанки base64; evt "voice" — чанки на приём.

---

## 8. Serial команды

| Команда | Описание |
|---------|----------|
| send &lt;text&gt; | Broadcast сообщение |
| send &lt;hex8&gt; &lt;text&gt; | Unicast на узел (hex8 = 8 hex-символов ID) |
| ping &lt;hex8&gt; | Проверка связи (отправить PING, ждать PONG) |
| route &lt;hex8&gt; | Запросить маршрут до узла (ROUTE_REQ) |
| region EU\|UK\|RU\|US\|AU | Установить регион |

---

## 9. Регионы

| Код | Частота | Мощность | Duty cycle |
|-----|---------|----------|------------|
| EU | 868.1 / 868.3 / 868.5 MHz (3 канала LoRaWAN) | 14 dBm | 1% (36 с/час) |
| UK | как EU | 14 dBm | 1% (36 с/час) |
| RU | 868.8 MHz | 20 dBm | — |
| US | 915.0 MHz | 22 dBm | — |
| AU | 915.0 MHz | 22 dBm | — |

**EU/UK:** выбор из 3 LoRaWAN-совместимых каналов (ETSI G1). Канал 0 = 868.1, 1 = 868.3, 2 = 868.5 MHz.

---

## 10. Радио

- **Модуль:** SX1262 (RadioLib)
- **Параметры:** SF7, BW 125 kHz, CR 5
- **Sync word:** 0x12 (приватная сеть)
- **CRC:** 2 байта
