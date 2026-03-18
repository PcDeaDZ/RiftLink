# RiftLink App (PWA)

Приложение для mesh-сети RiftLink. Работает в браузере (Chrome, Edge, Яндекс.Браузер на Android/ПК).

## Запуск

```bash
# Локальный сервер (нужен для Web Bluetooth)
npx serve .
# или
python -m http.server 8080
```

Откройте http://localhost:8080

## Функции

- **Чат** — сообщения, broadcast, unicast, группы
- **Сеть** — соседи, группы, маршруты
- **Карта** — локации узлов
- **Настройки** — никнейм, регион, OTA, отключение

## Требования

- Web Bluetooth (Chrome 56+, Edge, Samsung Internet)
- HTTPS или localhost
- Устройство RiftLink (Heltec LoRa) с BLE
