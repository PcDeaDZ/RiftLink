<p align="center">
  <img src="https://img.shields.io/badge/RiftLink-API_Reference-42A5F5?style=for-the-badge&logo=radio&logoColor=white" alt="RiftLink" />
</p>

# 📘 RiftLink — BLE и Serial API

> Справочник для разработчиков приложений и интеграций

<p align="center">
  <img src="https://img.shields.io/badge/BLE-GATT-0082FC?style=flat-square&logo=bluetooth" alt="BLE" />
  <img src="https://img.shields.io/badge/Serial-115200_8N1-888?style=flat-square" alt="Serial" />
  <img src="https://img.shields.io/badge/JSON-UTF--8-4CAF50?style=flat-square" alt="JSON" />
  <img src="https://img.shields.io/badge/Flutter-Dart-02569B?style=flat-square&logo=flutter" alt="Flutter" />
</p>

---

## 1. 📱 BLE GATT

| Параметр | Значение |
|----------|----------|
| Устройство | RiftLink |
| Сервис | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| TX (write, без ответа) | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| RX (notify) | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

**Порядок:** Подключение → discoverServices → setNotifyValue(true) на RX → write на TX.

---

## 2. 📤 Команды (приложение → устройство)

Все команды — JSON, UTF-8, запись в TX-характеристику.

### 2.1 send — отправить сообщение

**Broadcast:**
```json
{"cmd":"send","text":"Hello mesh!"}
```

**В группу** (group = ID группы, по умолчанию 1):
```json
{"cmd":"send","group":1,"text":"Hello group!"}
```

**Unicast** (to = 8 hex-символов Node ID):
```json
{"cmd":"send","to":"A1B2C3D4","text":"Привет"}
```

### 2.2 location — отправить геолокацию (broadcast)

```json
{"cmd":"location","lat":55.7558,"lon":37.6173,"alt":150}
```

### 2.3 ota — запустить OTA режим

Создаёт WiFi AP `RiftLink-OTA` (пароль `riftlink123`). Устройство переходит в режим ожидания прошивки.

```json
{"cmd":"ota"}
```

Ответ: `{"evt":"ota","ip":"192.168.4.1","ssid":"RiftLink-OTA","password":"riftlink123"}`

### 2.4 region — установить регион

```json
{"cmd":"region","region":"EU"}
```

Допустимые: `EU`, `UK`, `RU`, `US`, `AU`.

### 2.5 nickname — установить никнейм

```json
{"cmd":"nickname","nickname":"Alice"}
```

`nickname`: до 16 символов. Пустая строка — сброс никнейма.

### 2.6 channel — установить канал (EU/UK)

Для EU и UK доступны 3 LoRaWAN-канала: 0 (868.1 MHz), 1 (868.3 MHz), 2 (868.5 MHz).

```json
{"cmd":"channel","channel":0}
```

`channel`: 0, 1 или 2.

### 2.7 voice — голосовое сообщение (unicast)

Приложение записывает аудио, кодирует в Opus 8 kbps, отправляет чанками base64.

```json
{"cmd":"voice","to":"A1B2C3D4","chunk":0,"total":10,"data":"base64..."}
```

`chunk`: 0..total-1. `total`: число чанков. `data`: base64-encoded Opus. Макс. ~30 KB (30 сек).

### 2.8 info — запрос информации об устройстве

По запросу устройство отправляет `evt "info"` (id, nickname, region, freq, neighbors и т.д.).

```json
{"cmd":"info"}
```

### 2.9 read — подтверждение прочтения (unicast)

Приложение отправляет при просмотре входящего сообщения.

```json
{"cmd":"read","from":"A1B2C3D4E5F60708","msgId":123456}
```

`from` — отправитель сообщения (кому отправляем READ).

### 2.10 ping — отправка PING (проверка связи)

```json
{"cmd":"ping","to":"A1B2C3D4"}
```

Отправляет PING на узел. Получатель отвечает PONG. Приложение получит `{"evt":"pong","from":"..."}`.

### 2.11 invite — получить данные для QR-приглашения

```json
{"cmd":"invite"}
```

Ответ: `{"evt":"invite","id":"A1B2C3D4E5F60708","pubKey":"base64..."}` — Node ID и публичный ключ X25519 для QR.

### 2.12 acceptInvite — принять приглашение (сканирование QR)

```json
{"cmd":"acceptInvite","id":"A1B2C3D4E5F60708","pubKey":"base64..."}
```

Добавляет ключ узла и отправляет ему свой KEY_EXCHANGE. Формат QR: `riftlink:id:pubKey`.

Ответ: `{"evt":"region","region":"EU","freq":868.0,"power":14}`

---

## 3. 📥 События (устройство → приложение)

Подписка на RX (notify). Все события — JSON.

### 3.1 info — при подключении

```json
{"evt":"info","id":"A1B2C3D4E5F60708","nickname":"Alice","region":"EU","freq":868.1,"power":14,"channel":0,"version":"1.3.5"}
```

`nickname` — опционально. `channel` — только для EU/UK (0–2). `neighbors` — массив Node ID (hex) видимых соседей. `version` — версия прошивки (например 1.3.5).

### 3.2 msg — входящее сообщение

```json
{"evt":"msg","from":"A1B2C3D4E5F60708","text":"Hello!","msgId":123456,"rssi":-72}
```

`msgId` — только для unicast (для отправки READ при просмотре). `rssi` — уровень сигнала LoRa в dBm (опционально).

### 3.3 sent — отправлено (unicast поставлен в очередь)

```json
{"evt":"sent","to":"A1B2C3D4E5F60708","msgId":123456}
```

### 3.4 delivered — доставлено (ACK получен)

```json
{"evt":"delivered","from":"A1B2C3D4E5F60708","msgId":123456,"rssi":-72}
```

`from` — получатель (кто отправил ACK). `rssi` — уровень сигнала в dBm (опционально).

### 3.5 read — прочитано

```json
{"evt":"read","from":"A1B2C3D4E5F60708","msgId":123456,"rssi":-72}
```

`from` — получатель (кто отправил READ). `rssi` — уровень сигнала в dBm (опционально).

### 3.6 location — геолокация от узла

```json
{"evt":"location","from":"A1B2C3D4","lat":55.7558,"lon":37.6173,"alt":150,"rssi":-72}
```

`rssi` — уровень сигнала в dBm (опционально).

### 3.7 telemetry — телеметрия

```json
{"evt":"telemetry","from":"A1B2C3D4","battery":3850,"heapKb":180,"rssi":-72}
```

`battery` — напряжение в mV, `heapKb` — свободная heap в KB. `rssi` — уровень сигнала в dBm (опционально).

### 3.8 ota — OTA запущен

```json
{"evt":"ota","ip":"192.168.4.1","ssid":"RiftLink-OTA","password":"riftlink123"}
```

### 3.9 region — регион изменён

```json
{"evt":"region","region":"EU","freq":868.0,"power":14}
```

### 3.10 neighbors — обновление списка соседей

```json
{"evt":"neighbors","neighbors":["A1B2C3D4E5F60708","B2C3D4E5F6070819"]}
```

Отправляется при появлении нового узла (HELLO).

### 3.11 voice — голосовое сообщение (чанками)

```json
{"evt":"voice","from":"A1B2C3D4E5F60708","chunk":0,"total":20,"data":"base64..."}
```

Собрать все чанки, декодировать base64 → Opus, воспроизвести.

### 3.12 pong — ответ на PING (проверка связи)

```json
{"evt":"pong","from":"A1B2C3D4E5F60708","rssi":-72}
```

`rssi` — уровень сигнала в dBm (опционально).

---

## 4. ⌨️ Serial API (115200 8N1)

Команды вводятся в Serial Monitor, завершаются `\n`.

### 4.1 send

```
send Hello world
```
Broadcast.

```
send A1B2C3D4 Привет
```
Unicast на узел с ID `A1B2C3D4` (первые 4 байта в hex).

### 4.2 ping

```
ping A1B2C3D4
```

Отправляет PING на узел. Получатель отвечает PONG. При BLE-подключении приложение получит `{"evt":"pong","from":"..."}`.

### 4.3 region

```
region EU
region RU
region US
region AU
```

### 4.4 nickname

```
nickname Alice
nickname
```

Установить никнейм (до 16 символов) или сбросить (пустая строка после команды).

### 4.5 channel (EU/UK)

```
channel 0
channel 1
channel 2
```

Выбор канала LoRaWAN для EU/UK: 0 = 868.1 MHz, 1 = 868.3 MHz, 2 = 868.5 MHz.

---

## 5. 💻 Пример (Flutter/Dart)

```dart
// Отправка broadcast
await txChar.write(
  utf8.encode(jsonEncode({'cmd': 'send', 'text': 'Hello'})),
  withoutResponse: true,
);

// Подписка на события
await rxChar.setNotifyValue(true);
rxChar.lastValueStream.listen((value) {
  final json = jsonDecode(utf8.decode(value)) as Map<String, dynamic>;
  switch (json['evt']) {
    case 'msg':
      print('MSG from ${json['from']}: ${json['text']}');
      break;
    case 'info':
      print('Node ID: ${json['id']}, region: ${json['region']}');
      break;
    // ...
  }
});
```

---

## 6. 🔄 OTA Upload (PlatformIO)

После `{"cmd":"ota"}` и подключения к WiFi `RiftLink-OTA`:

```bash
cd firmware
pio run -t upload -e heltec_v3_ota
```

Или: `.\build_and_flash.ps1 -Flash -Ota`
