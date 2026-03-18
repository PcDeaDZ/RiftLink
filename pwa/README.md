# RiftLink Test PWA

Тестовое PWA для подключения к устройству RiftLink по Web Bluetooth.

## Поддержка

- **Chrome / Edge** на Android и desktop (Windows, macOS, Linux)
- **Требуется HTTPS** (или localhost для разработки)
- Safari на iOS **не поддерживает** Web Bluetooth

## Запуск

### Локально (localhost)

```bash
# Python
cd pwa && python -m http.server 8080

# Node.js (npx)
npx serve pwa -p 8080

# PHP
cd pwa && php -S localhost:8080
```

Откройте http://localhost:8080

### С телефона

1. Разместите PWA на HTTPS-сервере
2. Откройте URL в Chrome на Android
3. Меню → «Установить приложение» (Add to Home Screen)
4. Запустите установленное приложение

## Использование

1. **Подключиться** — выбор устройства RL-XXXXXXXX в диалоге Bluetooth
2. **Отправить** — broadcast или unicast (укажите Node ID в поле)
3. **Команды** — Info, Ping, Selftest, OTA, Регион

## Структура

```
pwa/
├── index.html
├── manifest.json
├── style.css
├── app.js
└── README.md
```
