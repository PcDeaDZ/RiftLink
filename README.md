# RiftLink (RL)

Собственный mesh-протокол для Heltec WiFi LoRa 32 (V3/V4). Независимое решение с E2E-шифрованием, Flutter-приложением и PWA.

**Версия:** прошивка 1.2.0, приложение 1.0.0

**Кратко:** LoRa mesh-сеть с текстовыми и голосовыми сообщениями, X25519 E2E, маршрутизацией, группами и офлайн-очередью. Прошивка на ESP32, приложение на Flutter (Android/iOS/Web).

## 📱 Платформы

| Плата | Дисплей | Сборка |
|-------|---------|--------|
| Heltec V3 | OLED 128×64 | `heltec_v3` |
| Heltec V4 | OLED 128×64 | `heltec_v4` |
| Heltec V3 Paper | E-Ink 2.13" | `heltec_v3_paper` |

## 📁 Структура

```
dual_boot/
├── firmware/       # Прошивка ESP32 (PlatformIO)
├── app/            # Flutter-приложение (Android, iOS, Web/PWA)
├── docs/           # Документация и спецификации
├── docs/plans/     # Планы, анализ, troubleshooting
├── scripts/        # Вспомогательные скрипты (Serial API тест)
└── build_and_flash.ps1
```

## 📚 Документация

- **[docs/CUSTOM_PROTOCOL_PLAN.md](docs/CUSTOM_PROTOCOL_PLAN.md)** — ТЗ, архитектура, этапы, текущее состояние
- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — спецификация протокола (формат пакетов, BLE, Serial)
- **[docs/API.md](docs/API.md)** — BLE/Serial API с примерами
- **[docs/RECOVERY.md](docs/RECOVERY.md)** — восстановление при «не включается»

**Планы и анализ** (docs/plans/):
- [ADDITIONAL_FEATURES_PLAN.md](docs/plans/ADDITIONAL_FEATURES_PLAN.md) — дополнительные фичи (poweroff, RSSI, автоотключение при низком заряде и др.)
- [PACKET_READ_ANALYSIS.md](docs/plans/PACKET_READ_ANALYSIS.md) — анализ коррупции пакетов при RF-помехах
- [PACKET_VALIDATION_PLAN.md](docs/plans/PACKET_VALIDATION_PLAN.md) — валидация и синхронизация пакетов

## 🚀 Быстрый старт

### Прошивка

```powershell
# Сборка
.\build_and_flash.ps1

# Сборка + прошивка
.\build_and_flash.ps1 -Flash

# Для Heltec V4
.\build_and_flash.ps1 -V4 -Flash

# Для Heltec V3 Paper (E-Ink)
.\build_and_flash.ps1 -V3Paper -Flash

# V4 не грузится? Безопасный конфиг (V3-совместимый)
.\build_and_flash.ps1 -V4Safe -Flash

# Восстановление (полная очистка + прошивка)
.\build_and_flash.ps1 -Flash -Erase

# Сборка + прошивка + монитор
.\build_and_flash.ps1 -Flash -Monitor

# OTA (обновление по WiFi): 1) BLE cmd {"cmd":"ota"} → 2) подключиться к WiFi RiftLink-OTA (пароль riftlink123) → 3) запустить:
.\build_and_flash.ps1 -Flash -Ota
```

Требуется: [PlatformIO](https://platformio.org/).

**Linux (Ubuntu/Debian):**
```bash
chmod +x build_and_flash.sh
./build_and_flash.sh              # Сборка V3
./build_and_flash.sh --v4 --flash # Сборка + прошивка V4
./build_and_flash.sh --app       # Сборка Flutter APK
```
Для прошивки по USB: `sudo usermod -aG dialout $USER` (перелогиньтесь).

Тест Serial API (Python):
```bash
pip install -r scripts/requirements.txt
python scripts/serial_test.py COM3
```

### Приложение (Flutter)

```bash
cd app
flutter pub get
flutter run -d chrome    # Web/PWA
flutter run -d android   # Android
flutter run -d ios       # iOS (macOS)
```

Сборка PWA:
```bash
flutter build web
# Результат в app/build/web/
```

Требуется: [Flutter SDK](https://flutter.dev/).

## ✅ Реализовано

| Функция | Описание |
|---------|----------|
| MSG | Текстовые сообщения (broadcast/unicast) |
| X25519 | E2E-шифрование на пару узлов (KEY_EXCHANGE) |
| GROUP_MSG | Групповые сообщения |
| Логические каналы | channel ID в заголовке |
| Офлайн‑очередь | Store-and-forward до 16 сообщений |
| ROUTE_REQ/REPLY | Проактивная маршрутизация (AODV-подобная) |
| VOICE_MSG | Голосовое сообщение (Opus 8 kbps, Flutter запись/воспроизведение) |
| ACK, READ, retransmit | ✓✓ доставлено, ✓✓✓ прочитано, до 3 повторов |
| LZ4, MSG_FRAG | Сжатие (≥50 байт), фрагментация до 2 KB |
| LOCATION, TELEMETRY | Геолокация, телеметрия (батарея, heap) |
| OTA, регионы | Обновление по WiFi, EU/UK/RU/US/AU |
| invite/acceptInvite | QR-приглашение по ссылке (присоединение к сети) |
| Валидация пакетов | Толерантность к RF-помехам, sync byte 0x5A, opcode-at-offset |
| Индикатор батареи | Bat N% на экране (Main) |

## 📋 Планы на будущее

| Приоритет | Фича | Описание |
|-----------|------|----------|
| Высокий | Скрытие вкладки GPS | Не показывать GPS, если модуль не обнаружен |
| Высокий | Автоотключение при низком заряде | Пороги 5/15/30%, защита батареи |
| Средний | Отображение RSSI | Сила сигнала соседей на экране |
| Средний | Часы / sleep | «sleep» вместо времени в спячке, не будить OLED от времени |
| Средний | Функция выключения | BLE cmd poweroff, long-press, VEXT |
| Низкий | Тест антенны | Проверка при включении |
| Низкий | Тест сигнала и трассировка | TX/RX тест, traceroute по mesh |
| Низкий | BLE: ключи сообщений | Локализация ошибок на клиенте (queue_full и др.) |

Подробнее: [docs/plans/ADDITIONAL_FEATURES_PLAN.md](docs/plans/ADDITIONAL_FEATURES_PLAN.md)

## 📄 Лицензия

См. проект.
