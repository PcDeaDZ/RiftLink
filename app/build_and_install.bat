@echo off
echo RiftLink - Build and Install APK
echo.

set "JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-17.0.18.8-hotspot"
set "ANDROID_HOME=C:\Users\HYPERPC\Android"
set "PATH=%JAVA_HOME%\bin;%PATH%"

set "FLUTTER_BAT=C:\Users\HYPERPC\Downloads\flutter_windows_3.41.4-stable\flutter\bin\flutter.bat"
set "ADB_BAT=%ANDROID_HOME%\platform-tools\adb.exe"

if not exist "%FLUTTER_BAT%" (
    where flutter >nul 2>nul
    if %errorlevel% neq 0 (
        echo Error: Flutter not found.
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
echo 2. flutter build apk --release --target-platform android-arm64...
call "%FLUTTER_BAT%" build apk --release --target-platform android-arm64
if %errorlevel% neq 0 (
    echo Build failed.
    pause
    exit /b 1
)

echo.
echo 3. adb install...
if not exist "%ADB_BAT%" (
    echo Error: ADB not found at %ADB_BAT%
    pause
    exit /b 1
)

call "%ADB_BAT%" install -r build\app\outputs\flutter-apk\app-release.apk
if %errorlevel% neq 0 (
    echo Install failed. Check that device is connected: adb devices
    pause
    exit /b 1
)

echo.
echo Done! APK built and installed.
pause
