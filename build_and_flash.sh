#!/bin/bash
# RiftLink (RL) — сборка и прошивка (Ubuntu/Debian)
#
# Требования: PlatformIO (pio), Flutter (для --app)
# Для прошивки по USB: добавьте пользователя в группу dialout
#   sudo usermod -aG dialout $USER  # перелогиньтесь после этого
#
# Использование: ./build_and_flash.sh [опции]
#   --all       — сборка для всех устройств (V3, Paper 1.0/1.1/1.2, V4, V4 Safe)
#   --v3        — Heltec WiFi LoRa 32 V3 (по умолчанию)
#   --v3paper   — Heltec WiFi LoRa 32 V3 Paper (e-paper)
#   --v4        — Heltec WiFi LoRa 32 V4
#   --v4safe    — V4 безопасный конфиг
#   --flash     — сборка + прошивка
#   --monitor   — после прошивки запустить монитор порта
#   --ota       — OTA upload (BLE cmd "ota" → WiFi RiftLink-OTA)
#   --erase     — полная очистка flash перед прошивкой (восстановление)
#   --app       — сборка Flutter APK (arm64)
# Прошивки: firmware/out/<env>/firmware.bin

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR/firmware"
APP_DIR="$SCRIPT_DIR/app"

# Параметры по умолчанию
FLASH=0
MONITOR=0
OTA=0
ALL=0
ERASE=0
APP_ONLY=0
ENV="heltec_v3"

# Парсинг аргументов
while [[ $# -gt 0 ]]; do
    case $1 in
        --flash)    FLASH=1; shift ;;
        --monitor)  MONITOR=1; shift ;;
        --ota)      OTA=1; shift ;;
        --all)      ALL=1; shift ;;
        --erase)    ERASE=1; shift ;;
        --app)      APP_ONLY=1; shift ;;
        --v3)       ENV="heltec_v3"; shift ;;
        --v3paper)  ENV="heltec_v3_paper"; shift ;;
        --v4)       ENV="heltec_v4"; shift ;;
        --v4safe)   ENV="heltec_v4_safe"; shift ;;
        *)          echo "Неизвестный параметр: $1"; exit 1 ;;
    esac
done

# Сборка Flutter APK
if [[ $APP_ONLY -eq 1 ]]; then
    echo -e "\033[36m[RiftLink] Сборка APK (arm64)...\033[0m"
    cd "$APP_DIR"
    flutter pub get
    flutter build apk --target-platform android-arm64 --no-tree-shake-icons
    if [[ -f "build/app/outputs/flutter-apk/app-release.apk" ]]; then
        SIZE=$(du -h "build/app/outputs/flutter-apk/app-release.apk" | cut -f1)
        echo -e "\033[32m[out] build/app/outputs/flutter-apk/app-release.apk ($SIZE)\033[0m"
    fi
    exit 0
fi

# Список env для мультисборки
ALL_ENVS=("heltec_v3" "heltec_v3_paper_auto" "heltec_v3_paper_v10" "heltec_v3_paper_v11" "heltec_v3_paper_v12" "heltec_v4" "heltec_v4_safe")

# OTA: замена env на _ota вариант
if [[ $OTA -eq 1 ]]; then
    case $ENV in
        heltec_v4_safe) ENV="heltec_v4_safe_ota" ;;
        heltec_v4)       ENV="heltec_v4_ota" ;;
        heltec_v3_paper) ENV="heltec_v3_paper_ota" ;;
        *)              ENV="heltec_v3_ota" ;;
    esac
fi

# Выбор env для сборки
if [[ $ALL -eq 1 ]]; then
    ENVS=("${ALL_ENVS[@]}")
    DO_FLASH=0  # При -all только сборка
else
    ENVS=("$ENV")
    DO_FLASH=$FLASH
fi

cd "$FIRMWARE_DIR"

for e in "${ENVS[@]}"; do
    if [[ $DO_FLASH -eq 1 ]]; then
        if [[ $OTA -eq 1 ]]; then
            echo -e "\033[33m[RiftLink] OTA upload ($e): подключитесь к WiFi RiftLink-OTA (пароль riftlink123)...\033[0m"
        fi
        if [[ $ERASE -eq 1 ]]; then
            echo -e "\033[33m[RiftLink] Полная очистка flash ($e)...\033[0m"
            pio run -e "$e" -t erase
        fi
        echo -e "\033[36m[RiftLink] Сборка и прошивка (env: $e)...\033[0m"
        pio run -e "$e" -t upload
    else
        echo -e "\033[36m[RiftLink] Сборка (env: $e)...\033[0m"
        pio run -e "$e"
    fi

    # Копирование firmware.bin в out/<env>/
    SRC=".pio/build/$e/firmware.bin"
    if [[ -f "$SRC" ]]; then
        OUT_DIR="out/$e"
        mkdir -p "$OUT_DIR"
        cp "$SRC" "$OUT_DIR/firmware.bin"
        echo -e "\033[32m[out] $e/firmware.bin\033[0m"
    fi
done

if [[ $MONITOR -eq 1 && ${#ENVS[@]} -eq 1 ]]; then
    echo -e "\033[36m[RiftLink] Монитор порта (Ctrl+C для выхода)...\033[0m"
    pio device monitor
fi
