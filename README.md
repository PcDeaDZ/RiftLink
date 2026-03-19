<p align="center">
  <img src="https://img.shields.io/badge/RiftLink-Mesh_Protocol-42A5F5?style=for-the-badge&logo=radio&logoColor=white" alt="RiftLink" />
</p>

<h1 align="center">📡 RiftLink</h1>
<p align="center">
  <strong>Собственный LoRa mesh-протокол</strong> — независимое решение с E2E-шифрованием, гарантированной доставкой и кроссплатформенным приложением
</p>

<p align="center">
  <img src="https://img.shields.io/badge/ESP32--S3-Firmware-E7352C?style=flat-square&logo=espressif" alt="ESP32" />
  <img src="https://img.shields.io/badge/LoRa-SX1262-00B0FF?style=flat-square&logo=lorawan" alt="LoRa" />
  <img src="https://img.shields.io/badge/Flutter-3.11+-02569B?style=flat-square&logo=flutter" alt="Flutter" />
  <img src="https://img.shields.io/badge/X25519-E2E_Encryption-4CAF50?style=flat-square" alt="E2E" />
  <img src="https://img.shields.io/badge/PlatformIO-Build-00979D?style=flat-square&logo=platformio" alt="PlatformIO" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-firmware_1.3.5_|_app_1.0.1-888?style=flat-square" alt="Version" />
  <img src="https://img.shields.io/badge/license-See_project-888?style=flat-square" alt="License" />
</p>

---

## 🎯 О проекте

**RiftLink** — это полноценный mesh-протокол для устройств **Heltec WiFi LoRa 32** (V3, V4, Paper). Не форк Meshtastic, а самостоятельная реализация с собственным стеком: от формата пакетов до BLE API и мобильного приложения.

### Почему RiftLink?

| ✨ | Описание |
|----|----------|
| 🔐 | **X25519 E2E-шифрование** — ключи обмениваются по KEY_EXCHANGE, сообщения шифруются ChaCha20-Poly1305 |
| 📨 | **ACK-based доставка** — unicast с retry по ACK, broadcast с отчётом «доставлено X/Y» |
| 📴 | **Офлайн-очередь** — store-and-forward до 16 сообщений в NVS, доставка при появлении узла |
| 🗺️ | **Проактивная маршрутизация** — AODV-подобный ROUTE_REQ/REPLY для mesh из 3+ узлов |
| 🎤 | **Голосовые сообщения** — Opus 8 kbps, запись и воспроизведение в приложении |
| 🌍 | **Регионы** — EU, UK, RU, US, AU с учётом регуляторики |
| 📶 | **BLE + Serial API** — управление с телефона или через Python/Serial |

---

## 📱 Поддерживаемые платы

| Плата | Дисплей | Сборка |
|-------|---------|--------|
| **Heltec V3** | OLED 128×64 | `heltec_v3` |
| **Heltec V4** | OLED 128×64, 16MB flash | `heltec_v4` |
| **Heltec V3 Paper** | E-Ink 2.13" | `heltec_v3_paper` |

---

## 🏗️ Структура репозитория

```
dual_boot/
├── firmware/       # Прошивка ESP32 (PlatformIO, C++)
├── app/            # Flutter-приложение (Android, iOS, Web/PWA)
├── docs/           # Спецификации, API, планы
├── scripts/        # Serial API тест (Python)
└── build_and_flash.ps1 / .sh
```

---

## 🚀 Быстрый старт

### Прошивка (Windows)

```powershell
# Сборка
.\build_and_flash.ps1

# Сборка + прошивка
.\build_and_flash.ps1 -Flash

# Heltec V4
.\build_and_flash.ps1 -V4 -Flash

# Heltec V3 Paper (E-Ink)
.\build_and_flash.ps1 -V3Paper -Flash

# OTA (WiFi): BLE → {"cmd":"ota"} → WiFi RiftLink-OTA → пароль riftlink123
.\build_and_flash.ps1 -Flash -Ota
```

**Требуется:** [PlatformIO](https://platformio.org/)

### Прошивка (Linux)

```bash
chmod +x build_and_flash.sh
./build_and_flash.sh              # Сборка V3
./build_and_flash.sh --v4 --flash # V4 + прошивка
./build_and_flash.sh --app       # Flutter APK
```

### Приложение (Flutter)

```bash
cd app
flutter pub get
flutter run -d chrome    # Web/PWA
flutter run -d android   # Android
flutter run -d ios       # iOS (macOS)
```

**Сборка PWA:** `flutter build web` → `app/build/web/`

---

## ✅ Реализованные функции

| Функция | Описание |
|---------|----------|
| 📝 **MSG** | Текстовые сообщения (broadcast, unicast, group) |
| 🔑 **X25519** | E2E-шифрование, KEY_EXCHANGE при первом контакте |
| 📢 **GROUP_MSG** | Групповые сообщения по ID группы |
| 📬 **ACK / READ** | ✓ отправлено, ✓✓ доставлено, ✓✓✓ прочитано, ✗ не доставлено |
| 🔄 **Retransmit** | ACK-based retry (до 4 попыток), затем offline_queue |
| 📴 **Офлайн-очередь** | До 16 сообщений в NVS, доставка при HELLO |
| 🛣️ **ROUTE_REQ/REPLY** | Проактивная маршрутизация (AODV-подобная) |
| 🎤 **VOICE_MSG** | Голос (Opus 8 kbps), фрагментация, TTL |
| 📦 **LZ4, MSG_FRAG** | Сжатие (≥50 байт), фрагментация до 2 KB |
| 📍 **LOCATION** | Геолокация (GPS), карта узлов |
| 📊 **TELEMETRY** | Батарея, heap, RSSI |
| 🔄 **OTA** | Обновление по WiFi (ArduinoOTA) |
| 🌐 **Регионы** | EU, UK, RU, US, AU |
| 🤝 **Invite/AcceptInvite** | QR-приглашение для присоединения к сети |
| 🛡️ **Валидация** | Толерантность к RF-помехам, sync byte 0x5A |

---

## 📚 Документация

| Документ | Описание |
|----------|----------|
| [CUSTOM_PROTOCOL_PLAN.md](docs/CUSTOM_PROTOCOL_PLAN.md) | Архитектура, спецификация, roadmap |
| [PROTOCOL.md](docs/PROTOCOL.md) | Формат пакетов, BLE, Serial |
| [API.md](docs/API.md) | BLE/Serial API с примерами |
| [RECOVERY.md](docs/RECOVERY.md) | Восстановление при «не включается» |

---

## 🧪 Тест Serial API (Python)

```bash
pip install -r scripts/requirements.txt
python scripts/serial_test.py COM3
```

---

## 📄 Лицензия

См. проект.
