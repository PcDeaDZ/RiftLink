# Сборка RiftLink для Android

## Требования

- **Flutter SDK** 3.16+ — [установка](https://docs.flutter.dev/get-started/install)
- **Android Studio** или **Android SDK** (command-line tools)
- **Java 17** (для Android Gradle)

## Сборка APK

```bash
cd app
flutter pub get
flutter build apk --release
```

APK будет в `app/build/app/outputs/flutter-apk/app-release.apk`.

## Сборка App Bundle (для Google Play)

```bash
flutter build appbundle --release
```

## Установка на устройство

```bash
flutter install
```

Или скопируйте `app-release.apk` на телефон и установите вручную.

## Android 15 (API 35)

Проект настроен на `targetSdk 35` и `compileSdk 35` — совместим с Android 15.

## Разрешения

Приложение запрашивает:
- Bluetooth (подключение к RiftLink)
- Геолокация (карта)
- Микрофон (голосовые сообщения)
