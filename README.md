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
  <img src="https://img.shields.io/badge/version-firmware_1.3.6_|_app_1.0.1-888?style=flat-square" alt="Version" />
  <img src="https://img.shields.io/badge/license-See_project-888?style=flat-square" alt="License" />
</p>

<p align="center">
  <a href="https://github.com/PcDeaDZ/RiftLink">GitHub</a>
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
├── pwa-app/        # PWA (Vite, Vitest) — веб-версия
├── docs/           # Спецификации, API, планы
├── scripts/        # Serial API тест, fix_encoding (кодировка build/install .ps1/.sh)
├── install.sh      # curl | bash — клон + setup (Linux/macOS)
├── install.ps1     # irm | iex — клон + setup (Windows)
├── build.ps1       # Менеджер (Windows): setup + build + flash + APK + монитор
├── build.sh        # То же для Linux/macOS
└── .env.example    # Шаблон путей → копируйте в .env.local
```

---

## ⚡ Установка «в один клик» (curl / wget)

Скрипт проверяет Git (устанавливает при необходимости), клонирует репозиторий и запускает `build --setup`:

**Linux / macOS / Git Bash:**
```bash
curl -fsSL https://raw.githubusercontent.com/PcDeaDZ/RiftLink/master/install.sh | bash
```
или
```bash
wget -qO- https://raw.githubusercontent.com/PcDeaDZ/RiftLink/master/install.sh | bash
```

**Windows (PowerShell):**
```powershell
irm https://raw.githubusercontent.com/PcDeaDZ/RiftLink/master/install.ps1 | iex
```

Папка по умолчанию: `~/riftlink` или `%USERPROFILE%\riftlink`. Переменные: `RIFTLINK_REPO`, `RIFTLINK_DIR`.

---

## 🛠️ Установка зависимостей (если уже клонировали)

**Windows:** `.\build.ps1 -Setup`  
**Linux/macOS:** `./build.sh --setup`

Или в меню: пункт 8 «Setup». Устанавливает: Python, pip-пакеты (pyserial, pytest, platformio), Flutter, проверяет Android SDK и Java. Пути — в `.env.local` (FLUTTER_ROOT, ANDROID_SDK_ROOT).

---

## 🚀 Быстрый старт

### Менеджер сборки (интерактивное меню)

**Windows:** `.\build.ps1`  
**Linux/macOS:** `./build.sh`

| Пункт | Действие |
|-------|----------|
| 1 | Сборка прошивки (выбор V3 / Paper / V4) |
| 2 | Прошивка (выбор порта + устройства) |
| 3 | Сборка + прошивка |
| 4 | Монитор порта (просмотр вывода устройства) |
| 5 | Сборка APK |
| 6 | Установка APK на устройство (adb) |
| 7 | Сборка + установка APK |
| 8 | Setup — установка зависимостей |
| 0 | Выход |

**Автоопределение портов:** CP210x → Paper, native USB (303A) → V4. При нескольких портах показываются подсказки. Перед прошивкой — вопрос об очистке flash. Скрипт не меняет текущую директорию.

### Прошивка (CLI)

```powershell
# Windows
.\build.ps1 -Setup                   # Установка зависимостей
.\build.ps1 -V4 -Flash               # Прошивка V4 (автовыбор порта)
.\build.ps1 -V3Paper -Flash -Erase   # Прошивка Paper с очисткой flash
.\build.ps1 -V4 -Flash -Port COM6    # Прошивка на указанный порт
.\build.ps1 -Monitor                 # Монитор порта (просмотр вывода)
.\build.ps1 -Monitor -Port COM6      # Монитор на указанный порт
.\build.ps1 -App                     # Сборка APK
.\build.ps1 -InstallApk              # Установка APK (выбор устройства)
.\build.ps1 -InstallApk -DeviceId 1  # Установка на устройство №1 из списка
```

```bash
# Linux/macOS
./build.sh --setup                   # Установка зависимостей
./build.sh --v4 --flash              # Прошивка V4
./build.sh --v3paper --flash --erase # Прошивка Paper с очисткой flash
./build.sh --v4 --flash --port /dev/ttyUSB0
./build.sh --monitor                 # Монитор порта
./build.sh --monitor --port /dev/ttyUSB0
./build.sh --app                     # Сборка APK
./build.sh --install                  # Установка APK (выбор устройства)
./build.sh --install --device R5CY123 # Установка на конкретное устройство
```

### Приложение (APK)

`.\build.ps1 -App -InstallApk` (Windows) или `./build.sh --app --install` (Linux/macOS)

**Требуется:** [PlatformIO](https://platformio.org/), Flutter, Android SDK

### Кодировка

После правок в `build.ps1`, `build.sh`, `install.ps1` или `install.sh` выполните `.\scripts\fix_encoding.ps1` — восстановит UTF-8 BOM (PowerShell) и LF (bash).

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
| 🤝 **Invite/AcceptInvite** | QR-приглашение, channelKey для приватных сетей |
| 🛡️ **Валидация** | Толерантность к RF-помехам, sync byte 0x5A |
| 📡 **NACK** | Запрос повтора по pktId (v2.1) |

---

## 📚 Документация

<p align="center">
  <img src="https://img.shields.io/badge/docs-4_docs-888?style=flat-square" alt="Docs" />
  <img src="https://img.shields.io/badge/spec-Protocol_%7C_API-42A5F5?style=flat-square" alt="Spec" />
</p>

| Документ | Описание |
|----------|----------|
| [📋 CUSTOM_PROTOCOL_PLAN.md](docs/CUSTOM_PROTOCOL_PLAN.md) | Архитектура, спецификация, roadmap |
| [📡 PROTOCOL.md](docs/PROTOCOL.md) | Формат пакетов, opcodes, BLE, Serial |
| [📘 API.md](docs/API.md) | BLE/Serial API с примерами |
| [🔧 RECOVERY.md](docs/RECOVERY.md) | Восстановление при «не включается» |
| [🌐 WEB_FLASH_GITHUB.md](docs/WEB_FLASH_GITHUB.md) | GitHub Pages веб‑прошивка через ESP Web Tools |

---

## 🧪 Тесты

| Компонент | Команда |
|-----------|---------|
| **Flutter** | `cd app && flutter test` |
| **PWA** | `cd pwa-app && npm test` |
| **Firmware (native)** | `cd firmware && pio test -e native` — 54 теста, требуется GCC (MSYS2 на Windows) |

Синтетические тесты проверяют: invite/acceptInvite с channelKey, парсинг BLE-событий, формат JSON, protocol::buildPacket/parsePacket. См. [firmware/test/README.md](firmware/test/README.md).

---

## 🧪 Тест Serial API (Python)

```bash
pip install -r scripts/requirements.txt
python scripts/serial_test.py COM3
```

---

## 📄 Лицензия

См. проект.
