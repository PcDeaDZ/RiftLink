@echo off
echo RiftLink - Building APK for Android
echo.

set "JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-17.0.18.8-hotspot"
set "ANDROID_HOME=C:\Users\HYPERPC\Android"
set "PATH=%JAVA_HOME%\bin;%PATH%"

set "FLUTTER_BAT=C:\Users\HYPERPC\Downloads\flutter_windows_3.41.4-stable\flutter\bin\flutter.bat"
if not exist "%FLUTTER_BAT%" (
    where flutter >nul 2>nul
    if %errorlevel% neq 0 (
        echo Error: Flutter not found.
        echo Set FLUTTER_BAT or add Flutter to PATH.
        pause
        exit /b 1
    )
    set "FLUTTER_BAT=flutter"
)

cd /d "%~dp0"

echo 1. flutter pub get...
call "%FLUTTER_BAT%" pub get
if %errorlevel% neq 0 exit /b 1

echo.
echo 2. flutter build apk --release...
call "%FLUTTER_BAT%" build apk --release
if %errorlevel% neq 0 (
    echo Build failed.
    pause
    exit /b 1
)

echo.
echo Done! APK: build\app\outputs\flutter-apk\app-release.apk
explorer build\app\outputs\flutter-apk
pause
