#!/bin/bash
# -*- coding: utf-8 -*-
# RiftLink — ультимативный менеджер (setup + build + flash + APK)
# UTF-8 для русского языка
export LANG="${LANG:-en_US.UTF-8}"
export LC_ALL="${LC_ALL:-en_US.UTF-8}"
# Использование: ./build.sh  или  ./build.sh --setup  или  ./build.sh --flash --v4
# Пути: .env.local (FLUTTER_ROOT, ANDROID_SDK_ROOT)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR/firmware"
APP_DIR="$SCRIPT_DIR/app"
ENV_FILE="$SCRIPT_DIR/.env.local"

# Загрузка .env.local
if [[ -f "$ENV_FILE" ]]; then
  set -a
  while IFS='=' read -r key value; do
    [[ "$key" =~ ^#.*$ ]] && continue
    [[ -z "$key" ]] && continue
    export "$key=$value"
  done < <(grep -v '^#' "$ENV_FILE" | grep -v '^[[:space:]]*$')
  set +a
fi

# Параметры
SETUP=0
UPDATE=0
FLASH=0
MONITOR=0
OTA=0
ALL=0
ERASE=0
APP_ONLY=0
INSTALL_APK=0
PORT=""
ENV=""
DEVICE_ID=""

# Парсинг
while [[ $# -gt 0 ]]; do
    case $1 in
        --setup)     SETUP=1; shift ;;
        --update)    UPDATE=1; shift ;;
        --flash)     FLASH=1; shift ;;
        --monitor)   MONITOR=1; shift ;;
        --ota)       OTA=1; shift ;;
        --all)       ALL=1; shift ;;
        --erase)     ERASE=1; shift ;;
        --app)       APP_ONLY=1; shift ;;
        --install)   INSTALL_APK=1; shift ;;
        --port)      PORT="$2"; shift 2 ;;
        --device)    DEVICE_ID="$2"; shift 2 ;;
        --v3)        ENV="heltec_v3"; shift ;;
        --v3paper)   ENV="heltec_v3_paper"; shift ;;
        --v4)        ENV="heltec_v4"; shift ;;
        *)           shift ;;
    esac
done

# Пути: .env.local → ANDROID_SDK_ROOT/ANDROID_HOME, FLUTTER_ROOT; иначе типичные
ANDROID_HOME="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
[[ -d "$HOME/Library/Android/sdk" ]] && ANDROID_HOME="$HOME/Library/Android/sdk"
ADB="$ANDROID_HOME/platform-tools/adb"
APK_PATH="$APP_DIR/build/app/outputs/flutter-apk/app-release.apk"
FLUTTER_CMD="${FLUTTER_ROOT:+$FLUTTER_ROOT/bin/flutter}"
FLUTTER_CMD="${FLUTTER_CMD:-flutter}"

# --- Установка Android SDK (command-line tools) ---
install_android_sdk() {
    local sdk_dir="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
    [[ "$(uname -s)" == "Darwin" ]] && sdk_dir="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
    local arch="linux"
    [[ "$(uname -s)" == "Darwin" ]] && arch="mac"
    local url="https://dl.google.com/android/repository/commandlinetools-${arch}-14742923_latest.zip"
    local tmpdir
    tmpdir="$(mktemp -d)"
    echo "  Скачивание Android command-line tools..."
    if ! command -v curl &>/dev/null && ! command -v wget &>/dev/null; then
        echo "[ОШИБКА] Нужен curl или wget для скачивания"
        rm -rf "$tmpdir"
        return 1
    fi
    if ! command -v unzip &>/dev/null; then
        echo "[ОШИБКА] Нужен unzip для распаковки. Установите: sudo apt install unzip" 2>/dev/null || echo "  brew install unzip"
        rm -rf "$tmpdir"
        return 1
    fi
    (cd "$tmpdir" && (curl -sSLf "$url" -o cmdlinetools.zip 2>/dev/null || wget -q "$url" -O cmdlinetools.zip)) || { rm -rf "$tmpdir"; return 1; }
    echo "  Распаковка в $sdk_dir..."
    mkdir -p "$sdk_dir"
    (cd "$tmpdir" && unzip -q cmdlinetools.zip)
    if [[ -d "$tmpdir/cmdline-tools" ]]; then
        mv "$tmpdir/cmdline-tools" "$sdk_dir/cmdline-tools-tmp"
        mkdir -p "$sdk_dir/cmdline-tools"
        mv "$sdk_dir/cmdline-tools-tmp" "$sdk_dir/cmdline-tools/latest"
    else
        mkdir -p "$sdk_dir/cmdline-tools/latest"
        mv "$tmpdir"/* "$sdk_dir/cmdline-tools/latest/" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
    export ANDROID_HOME="$sdk_dir"
    export PATH="$sdk_dir/cmdline-tools/latest/bin:$sdk_dir/platform-tools:$PATH"
    echo "  Установка platform-tools, build-tools, platforms..."
    yes | "$sdk_dir/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$sdk_dir" --install "platform-tools" "build-tools;34.0.0" "platforms;android-34" 2>/dev/null || true
    echo "  OK: $sdk_dir"
    [[ -f "$ENV_FILE" ]] || touch "$ENV_FILE"
    if grep -q "^ANDROID_SDK_ROOT=" "$ENV_FILE" 2>/dev/null; then
        sed "s|^ANDROID_SDK_ROOT=.*|ANDROID_SDK_ROOT=$sdk_dir|" "$ENV_FILE" > "$ENV_FILE.tmp" && mv "$ENV_FILE.tmp" "$ENV_FILE"
    else
        echo "ANDROID_SDK_ROOT=$sdk_dir" >> "$ENV_FILE"
    fi
}

# --- Установка Flutter SDK ---
install_flutter() {
    local flutter_dir="${FLUTTER_ROOT:-$HOME/flutter}"
    if [[ ! -d "$flutter_dir" ]]; then
        if ! command -v git &>/dev/null; then
            echo "[ОШИБКА] Git нужен для установки Flutter"
            return 1
        fi
        echo "  Скачивание Flutter (git clone)..."
        git clone --depth 1 https://github.com/flutter/flutter.git -b stable "$flutter_dir" || return 1
    fi
    export PATH="$flutter_dir/bin:$PATH"
    export FLUTTER_ROOT="$flutter_dir"
    echo "  flutter precache..."
    "$flutter_dir/bin/flutter" precache 2>/dev/null || true
    echo "  OK: $flutter_dir"
    [[ -f "$ENV_FILE" ]] || touch "$ENV_FILE"
    if grep -q "^FLUTTER_ROOT=" "$ENV_FILE" 2>/dev/null; then
        sed "s|^FLUTTER_ROOT=.*|FLUTTER_ROOT=$flutter_dir|" "$ENV_FILE" > "$ENV_FILE.tmp" && mv "$ENV_FILE.tmp" "$ENV_FILE"
    else
        echo "FLUTTER_ROOT=$flutter_dir" >> "$ENV_FILE"
    fi
}

# --- Setup (установка зависимостей) ---
do_setup() {
    echo ""
    echo -e "\033[36m========================================"
    echo "  RiftLink — установка зависимостей"
    echo -e "========================================\033[0m"
    echo ""

    # 1. Python
    echo -e "\033[33m[1/7] Python...\033[0m"
    PY_CMD=""
    command -v python3 &>/dev/null && PY_CMD="python3"
    command -v python &>/dev/null && PY_CMD="python"
    if [[ -z "$PY_CMD" ]]; then
        echo "[ОШИБКА] Python не найден. Установите: sudo apt install python3 python3-pip" 2>/dev/null || echo "  brew install python"
        return 1
    fi
    $PY_CMD --version
    echo ""

    # 2. pip
    echo -e "\033[33m[2/7] pip-пакеты...\033[0m"
    $PY_CMD -m pip install --upgrade pip -q 2>/dev/null
    $PY_CMD -m pip install -r "$SCRIPT_DIR/requirements-dev.txt" -q || $PY_CMD -m pip install --user -r "$SCRIPT_DIR/requirements-dev.txt" -q
    echo "  OK"
    echo ""

    # 3. PlatformIO
    echo -e "\033[33m[3/7] PlatformIO...\033[0m"
    PIO_CMD=""
    command -v pio &>/dev/null && PIO_CMD="pio"
    [[ -z "$PIO_CMD" ]] && PIO_CMD="$PY_CMD -m platformio"
    $PIO_CMD --version 2>/dev/null || { echo "[ОШИБКА] pip install -U platformio"; return 1; }
    echo ""

    # 4. Java
    echo -e "\033[33m[4/7] Java...\033[0m"
    command -v java &>/dev/null && java -version 2>&1 || echo "  Установите: sudo apt install openjdk-17-jdk" 2>/dev/null || echo "  brew install openjdk@17"
    echo ""

    # 5. Android SDK (сначала — нужен для flutter doctor)
    echo -e "\033[33m[5/7] Android SDK...\033[0m"
    if [[ -x "$ADB" ]]; then
        echo "  OK: $ANDROID_HOME"
    else
        echo "  Установка Android SDK..."
        if install_android_sdk; then
            ANDROID_HOME="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
            ADB="$ANDROID_HOME/platform-tools/adb"
        else
            echo "  Не удалось. Установите Android Studio или: export ANDROID_HOME=\$HOME/Android/Sdk"
        fi
    fi
    echo ""

    # 6. Flutter
    echo -e "\033[33m[6/7] Flutter...\033[0m"
    if "$FLUTTER_CMD" --version &>/dev/null; then
        "$FLUTTER_CMD" --version
        echo "  pub get..."
        (cd "$APP_DIR" && "$FLUTTER_CMD" pub get)
        yes | "$FLUTTER_CMD" doctor --android-licenses 2>/dev/null || true
    else
        echo "  Установка Flutter..."
        if install_flutter; then
            FLUTTER_CMD="$FLUTTER_ROOT/bin/flutter"
            "$FLUTTER_CMD" --version
            (cd "$APP_DIR" && "$FLUTTER_CMD" pub get)
            yes | "$FLUTTER_CMD" doctor --android-licenses 2>/dev/null || true
        else
            echo "  Не удалось. Установите вручную: sudo snap install flutter --classic" 2>/dev/null || echo "  brew install flutter"
        fi
    fi
    echo ""

    # 7. udev/dialout (Linux)
    echo -e "\033[33m[7/7] Проверка окружения...\033[0m"
    if [[ "$OSTYPE" == "linux-gnu"* ]] && ! groups | grep -q dialout; then
        echo "  Для доступа к COM: sudo usermod -aG dialout \$USER"
    fi
    echo ""
    echo -e "\033[32mГотово!\033[0m"
    echo "  Сборка: ./build.sh --v4"
    echo "  Прошивка: ./build.sh --v4 --flash"
    echo "  APK: ./build.sh --app --install"
    echo "  Обновление репо: ./update.sh   (перезапись локальных изменений)"
    echo ""
}

# --- Обновление из репозитория (перезапись всех локальных изменений) ---
do_update() {
    echo ""
    echo -e "\033[36m========================================"
    echo "  RiftLink — обновление из репозитория"
    echo "  (перезапись всех локальных изменений)"
    echo -e "========================================\033[0m"
    echo ""
    if [[ ! -d "$SCRIPT_DIR/.git" ]]; then
        echo -e "\033[31m[ОШИБКА] Не git-репозиторий: $SCRIPT_DIR\033[0m"
        return 1
    fi
    (cd "$SCRIPT_DIR" && git fetch origin && git reset --hard "origin/$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo main)" && git clean -fd)
    local ret=$?
    if [[ $ret -eq 0 ]]; then
        echo ""
        echo -e "\033[32mГотово! Репозиторий обновлён.\033[0m"
    fi
    return $ret
}

# Список портов (USB Serial, без Bluetooth) — формат: "PORT|HINT"
# pio: порт, -----, Hardware ID (BTHENUM=Bluetooth), Description
get_serial_ports() {
    (cd "$FIRMWARE_DIR" && pio device list 2>/dev/null) | awk '
        /^COM[0-9]+$/ || /^\/dev\/tty(USB|ACM|S)/ || /^\/dev\/cu\.usbmodem/ {
            if (port && !skip) print port "|" hint
            port=$0; hint="USB"; skip=0; next
        }
        /Hardware ID:.*BTHENUM/ { skip=1; next }
        /Hardware ID:.*303A|VID_303A|ESP32|JTAG/ || /Description:.*JTAG|USB Serial|debug/ { hint="V4 (native USB)"; next }
        /Hardware ID:.*CP210|10C4:EA60|Silicon/ || /Description:.*CP210|Silicon/ { hint="Paper (CP210x)"; next }
        END { if (port && !skip) print port "|" hint }
    ' | sort -u
}

get_adb_devices() {
    [[ -x "$ADB" ]] || return
    "$ADB" devices 2>/dev/null | awk '$2=="device" || $2=="unauthorized" {print $1}'
}

show_menu() {
    echo ""
    echo -e "\033[36m========================================"
    echo "  RiftLink — менеджер сборки"
    echo -e "========================================\033[0m"
    echo ""
    echo "  Прошивка (firmware):"
    echo "    1. Сборка прошивки (выбор устройства)"
    echo "    2. Прошивка (выбор порта + устройства)"
    echo "    3. Сборка + прошивка"
    echo "    4. Монитор порта"
    echo ""
    echo "  Приложение (APK):"
    echo "    5. Сборка APK"
    echo "    6. Установка APK на устройство (adb)"
    echo "    7. Сборка + установка APK"
    echo ""
    echo "  Окружение:"
    echo "    8. Setup — установка зависимостей (Python, PlatformIO, Flutter, Android)"
    echo "    9. Обновление из репозитория (перезапись локальных изменений)"
    echo ""
    echo "    0. Выход"
    echo ""
}

build_firmware() {
    local env_name="$1"
    echo -e "\033[36m[RiftLink] Сборка $env_name...\033[0m"
    (cd "$FIRMWARE_DIR" && pio run -e "$env_name")
    local ret=$?
    if [[ $ret -eq 0 ]]; then
        local src="$FIRMWARE_DIR/.pio/build/$env_name/firmware.bin"
        if [[ -f "$src" ]]; then
            mkdir -p "$FIRMWARE_DIR/out/$env_name"
            cp "$src" "$FIRMWARE_DIR/out/$env_name/firmware.bin"
            echo -e "\033[32m[out] $env_name/firmware.bin\033[0m"
        fi
    fi
    return $ret
}

flash_firmware() {
    local env_name="$1"
    local port="$2"
    local do_erase="${3:-0}"
    if [[ $do_erase -eq 1 ]]; then
        if [[ "$env_name" == *heltec* || "$env_name" == *paper* ]]; then
            # upload_erase: одна сборка + erase + upload (Heltec V3/V4/Paper)
            echo -e "\033[36m[RiftLink] Очистка flash + прошивка $env_name на $port...\033[0m"
            (cd "$FIRMWARE_DIR" && pio run -e "$env_name" -t upload_erase --upload-port "$port")
        else
            # FakeTech: одна сборка, затем erase и upload
            echo -e "\033[36m[RiftLink] Очистка flash + прошивка $env_name на $port...\033[0m"
            (cd "$FIRMWARE_DIR" && pio run -e "$env_name" -t erase -t upload --upload-port "$port")
        fi
    else
        echo -e "\033[36m[RiftLink] Прошивка $env_name на $port...\033[0m"
        (cd "$FIRMWARE_DIR" && pio run -e "$env_name" -t upload --upload-port "$port")
    fi
}

build_apk() {
    echo -e "\033[36m[RiftLink] Сборка APK...\033[0m"
    (cd "$APP_DIR" && "$FLUTTER_CMD" pub get && "$FLUTTER_CMD" build apk --release --target-platform android-arm64)
    if [[ -f "$APK_PATH" ]]; then
        local size=$(du -h "$APK_PATH" | cut -f1)
        echo -e "\033[32m[out] $APK_PATH ($size)\033[0m"
    fi
}

install_apk() {
    local device_id="$1"
    [[ -f "$APK_PATH" ]] || { echo -e "\033[31m[ОШИБКА] APK не найден. Сначала сборка (п.5).\033[0m"; return 1; }
    [[ -x "$ADB" ]] || { echo -e "\033[31m[ОШИБКА] ADB не найден. ANDROID_HOME=$ANDROID_HOME\033[0m"; return 1; }
    local devs=()
    while IFS= read -r d; do [[ -n "$d" ]] && devs+=("$d"); done < <(get_adb_devices)
    [[ ${#devs[@]} -eq 0 ]] && { echo -e "\033[31m[ОШИБКА] Нет подключённых устройств. adb devices\033[0m"; return 1; }
    local target="${device_id}"
    if [[ -z "$target" && ${#devs[@]} -gt 1 ]]; then
        echo ""
        echo "Подключённые устройства (adb devices):"
        "$ADB" devices 2>/dev/null
        echo ""
        for i in "${!devs[@]}"; do
            echo "  $((i+1)). ${devs[$i]}"
        done
        read -r -p "Выберите номер устройства (1-${#devs[@]}): " sel
        local idx=$((sel - 1))
        [[ $idx -ge 0 && $idx -lt ${#devs[@]} ]] && target="${devs[$idx]}"
    elif [[ -z "$target" ]]; then
        target="${devs[0]}"
    fi
    if [[ -n "$target" ]]; then
        "$ADB" -s "$target" get-state &>/dev/null || {
            echo -e "\033[31m[ОШИБКА] Устройство $target недоступно. adb devices:\033[0m"
            "$ADB" devices
            return 1
        }
        echo -e "\033[36m[RiftLink] Установка APK на $target...\033[0m"
        "$ADB" -s "$target" install -r "$APK_PATH"
    fi
}

# Режим CLI
if [[ $SETUP -eq 1 ]]; then
    do_setup
    exit $?
fi
if [[ $UPDATE -eq 1 ]]; then
    do_update
    exit $?
fi
# Монитор порта (просмотр вывода устройства)
if [[ $MONITOR -eq 1 && $FLASH -eq 0 && $APP_ONLY -eq 0 && $INSTALL_APK -eq 0 && $ALL -eq 0 && -z "$ENV" ]]; then
    mapfile -t ports < <(get_serial_ports)
    if [[ ${#ports[@]} -eq 0 ]]; then
        echo -e "\033[31m[ОШИБКА] Нет USB-портов. Подключите устройство.\033[0m"
        exit 1
    fi
    if [[ -n "$PORT" ]]; then
        monitor_port="$PORT"
    elif [[ ${#ports[@]} -eq 1 ]]; then
        monitor_port="${ports[0]%%|*}"
    else
        echo "Порты:"
        for i in "${!ports[@]}"; do
            p="${ports[$i]}"; echo "  $((i+1)). ${p/|/ - }"
        done
        read -r -p "Выберите порт (1-${#ports[@]}): " sel
        idx=$((sel - 1))
        [[ $idx -ge 0 && $idx -lt ${#ports[@]} ]] && monitor_port="${ports[$idx]%%|*}"
    fi
    if [[ -n "$monitor_port" ]]; then
        echo -e "\033[36m[RiftLink] Монитор $monitor_port (Ctrl+C — выход)\033[0m"
        (cd "$FIRMWARE_DIR" && pio device monitor --port "$monitor_port")
    fi
    exit 0
fi
if [[ $APP_ONLY -eq 1 && $INSTALL_APK -eq 1 ]]; then
    build_apk && install_apk "$DEVICE_ID"
    exit $?
fi
if [[ $ALL -eq 1 ]]; then
    ALL_ENVS=("heltec_v3" "heltec_v3_paper" "heltec_v4")
    for e in "${ALL_ENVS[@]}"; do
        build_firmware "$e"
    done
    exit 0
fi
if [[ $APP_ONLY -eq 1 ]]; then
    build_apk
    exit $?
fi
if [[ $INSTALL_APK -eq 1 ]]; then
    install_apk "$DEVICE_ID"
    exit $?
fi
if [[ $FLASH -eq 1 || -n "$ENV" ]]; then
    [[ -z "$ENV" ]] && ENV="heltec_v3"
    [[ $OTA -eq 1 ]] && case $ENV in
        heltec_v4)      ENV="heltec_v4_ota" ;;
        heltec_v3_paper) ENV="heltec_v3_paper_ota" ;;
        *)              ENV="heltec_v3_ota" ;;
    esac
    if [[ $FLASH -eq 1 ]]; then
        if [[ -z "$PORT" ]]; then
            mapfile -t ports < <(get_serial_ports)
            if [[ ${#ports[@]} -eq 0 ]]; then
                echo -e "\033[31m[ОШИБКА] Нет подключённых устройств.\033[0m"
                exit 1
            fi
            if [[ ${#ports[@]} -eq 1 ]]; then
                PORT="${ports[0]%%|*}"
            else
                echo "Порты (автоопределение):"
                for i in "${!ports[@]}"; do
                    p="${ports[$i]}"; echo "  $((i+1)). ${p/|/ - }"
                done
                read -r -p "Выберите порт: " sel
                idx=$((sel - 1))
                [[ $idx -ge 0 && $idx -lt ${#ports[@]} ]] && PORT="${ports[$idx]%%|*}"
            fi
        fi
        [[ -n "$PORT" ]] && flash_firmware "$ENV" "$PORT" "$ERASE"
    else
        build_firmware "$ENV"
    fi
    [[ $MONITOR -eq 1 ]] && (cd "$FIRMWARE_DIR" && pio device monitor)
    exit 0
fi

# Интерактивное меню
ENV_MAP=("heltec_v3:V3 (OLED)" "heltec_v3_paper:V3 Paper (e-ink)" "heltec_v4:V4 (OLED)")

while true; do
    show_menu
    read -r -p "Выберите действие: " choice
    case $choice in
        1)
            echo ""
            echo "  Выберите прошивку:"
            for i in "${!ENV_MAP[@]}"; do
                IFS=: read -r _ name <<< "${ENV_MAP[$i]}"
                echo "    $((i+1)). $name"
            done
            read -r -p "Номер: " e
            idx=$((e - 1))
            if [[ $idx -ge 0 && $idx -lt ${#ENV_MAP[@]} ]]; then
                IFS=: read -r env_name _ <<< "${ENV_MAP[$idx]}"
                build_firmware "$env_name"
            fi
            ;;
        2)
            echo ""
            echo -e "\033[36m  Прошивка — все параметры сразу:\033[0m"
            echo ""
            echo "  1. Прошивка:"
            for i in "${!ENV_MAP[@]}"; do
                IFS=: read -r _ name <<< "${ENV_MAP[$i]}"
                echo "     $((i+1)). $name"
            done
            read -r -p "     Номер: " e
            idx=$((e - 1))
            if [[ $idx -ge 0 && $idx -lt ${#ENV_MAP[@]} ]]; then
                IFS=: read -r env_name _ <<< "${ENV_MAP[$idx]}"
                mapfile -t ports < <(get_serial_ports)
                if [[ ${#ports[@]} -eq 0 ]]; then
                    echo -e "\033[31m[ОШИБКА] Нет USB-портов. Bluetooth не подходит.\033[0m"
                else
                    echo ""
                    echo "  2. Порт:"
                    prefer_paper=0; prefer_v4=0
                    [[ "$env_name" == *paper* ]] && prefer_paper=1
                    [[ "$env_name" == *v4* ]] && prefer_v4=1
                    for i in "${!ports[@]}"; do
                        p="${ports[$i]}"; pt="${p%%|*}"; ht="${p#*|}"
                        mark=""
                        [[ $prefer_paper -eq 1 && "$ht" == *Paper* ]] && mark=" <- рекомендуется"
                        [[ $prefer_v4 -eq 1 && "$ht" == *V4* ]] && mark=" <- рекомендуется"
                        echo "     $((i+1)). $pt - $ht$mark"
                    done
                    read -r -p "     Номер порта (1-${#ports[@]}): " pSel
                    pIdx=$((pSel - 1))
                    port=""
                    [[ $pIdx -ge 0 && $pIdx -lt ${#ports[@]} ]] && port="${ports[$pIdx]%%|*}"
                    echo ""
                    echo "  3. Очистить flash перед прошивкой? (y/n)"
                    read -r -p "     " yn
                    do_erase=0; [[ "$yn" =~ ^[yY] ]] && do_erase=1
                    echo ""
                    [[ -n "$port" ]] && flash_firmware "$env_name" "$port" "$do_erase"
                fi
            fi
            ;;
        3)
            echo ""
            echo -e "\033[36m  Сборка + прошивка — все параметры сразу:\033[0m"
            echo ""
            echo "  1. Прошивка:"
            for i in "${!ENV_MAP[@]}"; do
                IFS=: read -r _ name <<< "${ENV_MAP[$i]}"
                echo "     $((i+1)). $name"
            done
            read -r -p "     Номер: " e
            idx=$((e - 1))
            if [[ $idx -ge 0 && $idx -lt ${#ENV_MAP[@]} ]]; then
                IFS=: read -r env_name _ <<< "${ENV_MAP[$idx]}"
                mapfile -t ports < <(get_serial_ports)
                if [[ ${#ports[@]} -eq 0 ]]; then
                    echo -e "\033[31m[ОШИБКА] Нет USB-портов. Bluetooth не подходит.\033[0m"
                else
                    echo ""
                    echo "  2. Порт:"
                    prefer_paper=0; prefer_v4=0
                    [[ "$env_name" == *paper* ]] && prefer_paper=1
                    [[ "$env_name" == *v4* ]] && prefer_v4=1
                    for i in "${!ports[@]}"; do
                        p="${ports[$i]}"; pt="${p%%|*}"; ht="${p#*|}"
                        mark=""
                        [[ $prefer_paper -eq 1 && "$ht" == *Paper* ]] && mark=" <- рекомендуется"
                        [[ $prefer_v4 -eq 1 && "$ht" == *V4* ]] && mark=" <- рекомендуется"
                        echo "     $((i+1)). $pt - $ht$mark"
                    done
                    read -r -p "     Номер порта (1-${#ports[@]}): " pSel
                    pIdx=$((pSel - 1))
                    port=""
                    [[ $pIdx -ge 0 && $pIdx -lt ${#ports[@]} ]] && port="${ports[$pIdx]%%|*}"
                    echo ""
                    echo "  3. Очистить flash перед прошивкой? (y/n)"
                    read -r -p "     " yn
                    do_erase=0; [[ "$yn" =~ ^[yY] ]] && do_erase=1
                    echo ""
                    [[ -n "$port" ]] && flash_firmware "$env_name" "$port" "$do_erase"
                fi
            fi
            ;;
        4)
            echo ""
            echo -e "\033[36m  Монитор порта:\033[0m"
            mapfile -t ports < <(get_serial_ports)
            if [[ ${#ports[@]} -eq 0 ]]; then
                echo -e "\033[31m[ОШИБКА] Нет портов.\033[0m"
            else
                echo ""
                for i in "${!ports[@]}"; do
                    p="${ports[$i]}"; echo "  $((i+1)). ${p/|/ - }"
                done
                read -r -p "Порт (1-${#ports[@]}): " pSel
                pIdx=$((pSel - 1))
                port="${ports[0]%%|*}"
                [[ $pIdx -ge 0 && $pIdx -lt ${#ports[@]} ]] && port="${ports[$pIdx]%%|*}"
                echo ""
                (cd "$FIRMWARE_DIR" && pio device monitor --port "$port")
            fi
            ;;
        5) build_apk ;;
        6) install_apk ;;
        7) build_apk && install_apk ;;
        8) do_setup ;;
        9) do_update ;;
        0) exit 0 ;;
        *) echo "Неверный выбор." ;;
    esac
    echo ""
    read -r -p "Нажмите Enter для продолжения"
done
