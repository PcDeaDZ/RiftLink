# RiftLink (RL)

Собственный mesh-протокол для Heltec WiFi LoRa 32 (V3/V4). Независимое решение с E2E-шифрованием, Flutter-приложением и PWA.

**Кратко:** LoRa mesh-сеть с текстовыми и голосовыми сообщениями, X25519 E2E, маршрутизацией, группами и офлайн-очередью. Прошивка на ESP32, приложение на Flutter (Android/iOS/Web).

## Структура

```
dual_boot/
├── firmware/       # Прошивка ESP32 (PlatformIO)
├── app/            # Flutter-приложение (Android, iOS, Web/PWA)
├── docs/           # Документация и план
├── scripts/        # Вспомогательные скрипты (Serial API тест)
└── build_and_flash.ps1
```

## Документация

- **[docs/CUSTOM_PROTOCOL_PLAN.md](docs/CUSTOM_PROTOCOL_PLAN.md)** — ТЗ, архитектура, этапы
- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — спецификация протокола (формат пакетов, BLE, Serial)
- **[docs/API.md](docs/API.md)** — BLE/Serial API с примерами
- **[docs/RECOVERY.md](docs/RECOVERY.md)** — восстановление при «не включается»

## Быстрый старт

### Прошивка

```powershell
# Сборка
.\build_and_flash.ps1

# Сборка + прошивка
.\build_and_flash.ps1 -Flash

# Для Heltec V4
.\build_and_flash.ps1 -V4 -Flash

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

## Реализовано

| Функция | Описание |
|---------|----------|
| MSG | Текстовые сообщения (broadcast/unicast) |
| X25519 | E2E-шифрование на пару узлов (KEY_EXCHANGE) |
| GROUP_MSG | Групповые сообщения |
| Логические каналы | channel ID в заголовке |
| Офлайн‑очередь | Store-and-forward до 16 сообщений |
| ROUTE_REQ/REPLY | Проактивная маршрутизация |
| VOICE_MSG | Голосовое сообщение (Opus, Flutter запись/воспроизведение) |
| ACK, retransmit | Подтверждение и retransmit |
| LZ4, MSG_FRAG | Сжатие, фрагментация длинных сообщений |
| LOCATION, TELEMETRY | Геолокация, телеметрия |
| OTA, регионы | Обновление по WiFi, EU/UK/RU/US/AU |

## Лицензия

См. проект.
