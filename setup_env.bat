@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
echo.
echo ========================================
echo   RiftLink — установка зависимостей
echo ========================================
echo.

cd /d "%~dp0"

:: --- 1. Python ---
echo [1/7] Проверка Python...
where python >nul 2>nul
if %errorlevel% neq 0 (
    where py >nul 2>nul
    if !errorlevel! equ 0 (
        echo      Найден py launcher. Используем py -3...
        set "PY_CMD=py -3"
    ) else (
        echo.
        echo Python не найден. Установка через winget...
        winget install Python.Python.3.12 --accept-package-agreements --accept-source-agreements
        if !errorlevel! equ 0 (
            echo      Python установлен. Перезапустите терминал и запустите setup_env.bat снова.
        ) else (
            echo [ОШИБКА] Установите Python 3.10+ с https://www.python.org/
            echo          Или: winget install Python.Python.3.12
        )
        pause
        exit /b 1
    )
) else (
    set "PY_CMD=python"
)
%PY_CMD% --version
echo.

:: --- 2. pip + зависимости ---
echo [2/7] Установка pip-пакетов (pyserial, pytest, platformio)...
%PY_CMD% -m pip install --upgrade pip -q
%PY_CMD% -m pip install -r requirements-dev.txt -q
if %errorlevel% neq 0 (
    echo [ОШИБКА] Не удалось установить pip-пакеты.
    pause
    exit /b 1
)
echo      OK: pyserial, pytest, platformio
echo.

:: --- 3. PlatformIO ---
echo [3/7] Проверка PlatformIO...
where pio >nul 2>nul
if %errorlevel% neq 0 (
    %PY_CMD% -m platformio --version >nul 2>nul
    if !errorlevel! equ 0 (
        set "PIO_CMD=%PY_CMD% -m platformio"
    ) else (
        echo [ОШИБКА] PlatformIO не найден. Переустановите: pip install -U platformio
        pause
        exit /b 1
    )
) else (
    set "PIO_CMD=pio"
)
%PIO_CMD% --version
echo.

:: --- 4. Java (нужен до Flutter/Android) ---
echo [4/7] Проверка Java...
set "JAVA_OK=0"
where java >nul 2>nul
if %errorlevel% equ 0 (
    set "JAVA_OK=1"
    java -version 2>nul
)
if %JAVA_OK% equ 0 (
    echo      Установка Java 17 через winget...
    winget install EclipseAdoptium.Temurin.17.JDK --accept-package-agreements --accept-source-agreements
    if !errorlevel! equ 0 (
        echo      Java установлен. Перезапустите терминал для обновления PATH.
    ) else (
        echo      Ошибка установки. Вручную: winget install EclipseAdoptium.Temurin.17.JDK
    )
)
echo.

:: --- 5. Flutter ---
echo [5/7] Проверка Flutter...
set "FLUTTER_OK=0"
set "FLUTTER_BIN="
where flutter >nul 2>nul
if %errorlevel% equ 0 (
    set "FLUTTER_OK=1"
    set "FLUTTER_BIN=flutter"
)
if %FLUTTER_OK% equ 0 (
    for %%D in ("%USERPROFILE%\flutter" "%USERPROFILE%\development\flutter" "C:\flutter" "C:\src\flutter") do (
        if exist "%%~D\bin\flutter.bat" (
            set "FLUTTER_OK=1"
            set "FLUTTER_BIN=%%~D\bin\flutter.bat"
        )
    )
)
if %FLUTTER_OK% equ 0 (
    echo      Установка Flutter через winget...
    winget install Google.Flutter --accept-package-agreements --accept-source-agreements
    if !errorlevel! equ 0 (
        echo      Flutter установлен. Перезапустите терминал и запустите setup_env.bat снова.
    ) else (
        echo      winget не сработал. Пробуем git clone...
        where git >nul 2>nul
        if !errorlevel! equ 0 (
            if not exist "%USERPROFILE%\flutter" (
                git clone https://github.com/flutter/flutter.git -b stable "%USERPROFILE%\flutter"
                set "PATH=%USERPROFILE%\flutter\bin;%PATH%"
                set "FLUTTER_BIN=%USERPROFILE%\flutter\bin\flutter.bat"
                set "FLUTTER_OK=1"
            )
        )
        if !FLUTTER_OK! equ 0 (
            echo      Скачайте Flutter с https://docs.flutter.dev/get-started/install
        )
    )
)
if %FLUTTER_OK% equ 1 (
    if defined FLUTTER_BIN (
        "%FLUTTER_BIN%" --version
        echo      Получение зависимостей Flutter (app)...
        cd app
        "%FLUTTER_BIN%" pub get
        cd ..
    ) else (
        flutter --version
        cd app
        flutter pub get
        cd ..
    )
)
echo.

:: --- 6. Android SDK ---
echo [6/7] Проверка Android SDK...
set "ANDROID_SDK_ROOT="
if defined ANDROID_HOME set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
if not defined ANDROID_SDK_ROOT set "ANDROID_SDK_ROOT=%LOCALAPPDATA%\Android\Sdk"
if not exist "%ANDROID_SDK_ROOT%" set "ANDROID_SDK_ROOT=%USERPROFILE%\Android\Sdk"

set "ANDROID_OK=0"
if exist "%ANDROID_SDK_ROOT%\platform-tools\adb.exe" set "ANDROID_OK=1"
if exist "%ANDROID_SDK_ROOT%\platforms" set "ANDROID_OK=1"

if %ANDROID_OK% equ 0 (
    echo      Android SDK не найден. Установка через winget (Android Studio)...
    winget install Google.AndroidStudio --accept-package-agreements --accept-source-agreements
    if !errorlevel! equ 0 (
        echo      Android Studio установлен. SDK будет в %%LOCALAPPDATA%%\Android\Sdk
        echo      Запустите Android Studio один раз для завершения настройки SDK.
    ) else (
        echo      Пробуем установить только Android Command Line Tools...
        set "ANDROID_SDK_ROOT=%USERPROFILE%\Android\Sdk"
        if not exist "%ANDROID_SDK_ROOT%" mkdir "%ANDROID_SDK_ROOT%"
        set "CMDLINE_ZIP=%TEMP%\commandlinetools-win.zip"
        set "CMDLINE_URL=https://dl.google.com/android/repository/commandlinetools-win-14742923_latest.zip"
        echo      Скачивание cmdline-tools...
        powershell -NoProfile -Command "Invoke-WebRequest -Uri '%CMDLINE_URL%' -OutFile '%CMDLINE_ZIP%' -UseBasicParsing"
        if exist "%CMDLINE_ZIP%" (
            echo      Распаковка...
            powershell -NoProfile -Command "Expand-Archive -Path '%CMDLINE_ZIP%' -DestinationPath '%TEMP%\android-cmdline' -Force"
            if exist "%TEMP%\android-cmdline\cmdline-tools" (
                if not exist "%ANDROID_SDK_ROOT%\cmdline-tools\latest" mkdir "%ANDROID_SDK_ROOT%\cmdline-tools\latest"
                xcopy "%TEMP%\android-cmdline\cmdline-tools\*" "%ANDROID_SDK_ROOT%\cmdline-tools\latest\" /E /I /Y >nul
                rd /s /q "%TEMP%\android-cmdline" 2>nul
                set "PATH=%ANDROID_SDK_ROOT%\cmdline-tools\latest\bin;%PATH%"
                echo      Установка platforms и build-tools...
                echo y | "%ANDROID_SDK_ROOT%\cmdline-tools\latest\bin\sdkmanager.bat" --sdk_root="%ANDROID_SDK_ROOT%" --install "platforms;android-35" "platforms;android-36" "build-tools;35.0.0" "platform-tools"
                set "ANDROID_OK=1"
                setx ANDROID_HOME "%ANDROID_SDK_ROOT%" >nul 2>nul
                echo      ANDROID_HOME установлен в переменные среды.
            )
            del "%CMDLINE_ZIP%" 2>nul
        )
    )
) else (
    echo      ANDROID_HOME=%ANDROID_SDK_ROOT%
    echo      OK: Android SDK найден
)
echo.

:: --- 7. Итог ---
echo [7/7] Проверка окружения...
echo.

echo ========================================
echo   Готово!
echo ========================================
echo.
echo Сборка прошивки:    cd firmware ^&^& pio run -e heltec_v4
echo Прошивка:           cd firmware ^&^& pio run -e heltec_v4 -t upload
echo Сборка APK:         cd app ^&^& flutter build apk --release
echo Тесты:              pytest
echo Обновление репо:    update.bat   (перезапись локальных изменений)
echo.
echo Если сборка APK падает: flutter doctor --android-licenses
echo.
pause
