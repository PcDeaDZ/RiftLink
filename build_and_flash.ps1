# RiftLink (RL) — сборка и прошивка
# Использование: .\build_and_flash.ps1 [-Flash] [-Monitor]
#   -All       — сборка для всех устройств (V3, Paper 1.0/1.1/1.2, V4, V4 Safe)
#   -V3        — Heltec WiFi LoRa 32 V3 (по умолчанию)
#   -V3Paper   — Heltec WiFi LoRa 32 V3 Paper (e-paper)
#   -V4        — Heltec WiFi LoRa 32 V4
#   -V4Safe    — V4 безопасный конфиг
# Прошивки: firmware/out/<env>/firmware.bin

param(
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Ota,      # OTA upload: сначала BLE cmd {"cmd":"ota"}, затем подключиться к WiFi RiftLink-OTA
    [switch]$All,      # Сборка для всех устройств (V3, V3 Paper, V4, V4 Safe)
    [switch]$V3,       # Heltec V3 (по умолчанию при одиночной сборке)
    [switch]$V3Paper,  # Heltec V3 Paper (e-paper)
    [switch]$V4,       # Heltec V4
    [switch]$V4Safe,   # V4: безопасный конфиг
    [switch]$Erase,    # Полная очистка flash перед прошивкой (восстановление)
    [switch]$App,      # Сборка Flutter APK (arm64, ~18 MB)
    [string]$Env = ""
)

# UTF-8 для корректного отображения русского в консоли
chcp 65001 | Out-Null
$OutputEncoding = [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Список env для мультисборки (-All): V3, Paper 1.0/1.1/1.2, V4, V4 Safe
$AllEnvs = @("heltec_v3", "heltec_v3_paper_auto", "heltec_v3_paper_v10", "heltec_v3_paper_v11", "heltec_v3_paper_v12", "heltec_v4", "heltec_v4_safe")

# Выбор env
if ($All) {
    $Envs = $AllEnvs
} else {
    if ($V4Safe) { $Env = "heltec_v4_safe" }
    elseif ($V4) { $Env = "heltec_v4" }
    elseif ($V3Paper) { $Env = "heltec_v3_paper" }
    elseif ($V3 -or $Env -eq "") { $Env = "heltec_v3" }
    if ($Ota) {
        $Env = switch ($Env) {
            "heltec_v4_safe" { "heltec_v4_safe_ota" }
            "heltec_v4" { "heltec_v4_ota" }
            "heltec_v3_paper" { "heltec_v3_paper_ota" }  # v12
            default { "heltec_v3_ota" }
        }
    }
    $Envs = @($Env)
}

# -App: только сборка Flutter APK
if ($App) {
    $AppDir = Join-Path $PSScriptRoot "app"
    $Flutter = "C:\Users\HYPERPC\Downloads\flutter_windows_3.41.4-stable\flutter\bin\flutter.bat"
    if (-not (Test-Path $Flutter)) { $Flutter = "flutter" }
    $env:ANDROID_HOME = "C:\Users\HYPERPC\Android"
    Set-Location $AppDir
    Write-Host "[RiftLink] Сборка APK (arm64)..." -ForegroundColor Cyan
    & $Flutter build apk --target-platform android-arm64 --no-tree-shake-icons
    if ($LASTEXITCODE -eq 0) {
        $apkDir = Join-Path $AppDir "build\app\outputs\flutter-apk"
        $apk = Join-Path $apkDir "app-release.apk"
        if (Test-Path $apk) {
            $size = [math]::Round((Get-Item $apk).Length / 1MB, 1)
            Write-Host "[out] $apk ($size MB)" -ForegroundColor Green
        }
    }
    exit $LASTEXITCODE
}

$FirmwareDir = Join-Path $PSScriptRoot "firmware"
Set-Location $FirmwareDir

# При -All только сборка (прошивка — для одного устройства: -V3 -Flash и т.д.)
$doFlash = $Flash -and -not $All

foreach ($e in $Envs) {
    if ($doFlash) {
        if ($Ota) {
            Write-Host "[RiftLink] OTA upload ($e): подключитесь к WiFi RiftLink-OTA (пароль riftlink123)..." -ForegroundColor Yellow
        }
        if ($Erase) {
            Write-Host "[RiftLink] Полная очистка flash ($e)..." -ForegroundColor Yellow
            pio run -e $e -t erase
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        Write-Host "[RiftLink] Сборка и прошивка (env: $e)..." -ForegroundColor Cyan
        pio run -e $e -t upload
    } else {
        Write-Host "[RiftLink] Сборка (env: $e)..." -ForegroundColor Cyan
        pio run -e $e
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    # Копирование firmware.bin в out/<env>/
    $src = Join-Path $FirmwareDir ".pio\build\$e\firmware.bin"
    if (Test-Path $src) {
        $outDir = Join-Path $FirmwareDir "out\$e"
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        Copy-Item $src (Join-Path $outDir "firmware.bin") -Force
        Write-Host "[out] $e/firmware.bin" -ForegroundColor Green
    }
}

if ($Monitor -and $Envs.Count -eq 1) {
    Write-Host "[RiftLink] Монитор порта (Ctrl+C для выхода)..." -ForegroundColor Cyan
    pio device monitor
}
