# RiftLink — ультимативный менеджер (setup + build + flash + APK)
# Использование: .\build.ps1  или  .\build.ps1 -Setup  или  .\build.ps1 -Flash -V4
# Пути: .env.local (FLUTTER_ROOT, ANDROID_SDK_ROOT)

param(
    [switch]$Setup,     # Установка зависимостей (Python, PlatformIO, Flutter, Android SDK)
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Ota,
    [switch]$All,      # Сборка для всех env (V3, Paper, V4)
    [switch]$FakeTech, # Сборка/прошивка FakeTech V5 (nRF52)
    [switch]$Erase,    # Очистка flash перед прошивкой
    [switch]$V3,
    [switch]$V3Paper,
    [switch]$V4,
    [switch]$App,
    [switch]$InstallApk,
    [string]$Port = "",
    [string]$BuildEnv = "",
    [string]$DeviceId = ""   # ID устройства adb для -InstallApk
)

# UTF-8 для русского языка (без BOM)
chcp 65001 | Out-Null
$OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::InputEncoding = [System.Text.Encoding]::UTF8

$ErrorActionPreference = "Stop"
$ScriptRoot = $PSScriptRoot
$FirmwareDir = Join-Path $ScriptRoot "firmware"
$AppDir = Join-Path $ScriptRoot "app"
$StartDir = Get-Location

# Загрузка .env.local
$envFile = Join-Path $ScriptRoot ".env.local"
if (Test-Path $envFile) {
    Get-Content $envFile -Encoding UTF8 | Where-Object { $_ -match '^\s*([^#][^=]+)=(.*)$' } | ForEach-Object {
        if ($_ -match '^\s*([^#][^=]+)=(.*)$') {
            $k = $Matches[1].Trim(); $v = $Matches[2].Trim()
            if ($k -eq "FLUTTER_ROOT") { $env:FLUTTER_ROOT = $v }
            if ($k -eq "ANDROID_SDK_ROOT") { $env:ANDROID_SDK_ROOT = $v; $env:ANDROID_HOME = $v }
        }
    }
}

# Пути: .env.local → ANDROID_HOME/ANDROID_SDK_ROOT, FLUTTER_ROOT; иначе типичные
$env:ANDROID_HOME = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
$Adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
$Flutter = if ($env:FLUTTER_ROOT -and (Test-Path (Join-Path $env:FLUTTER_ROOT "bin\flutter.bat"))) {
    Join-Path $env:FLUTTER_ROOT "bin\flutter.bat"
} elseif (Get-Command flutter -ErrorAction SilentlyContinue) { "flutter" } else {
    $f = @("$env:USERPROFILE\flutter\bin\flutter.bat", "C:\flutter\bin\flutter.bat") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($f) { $f } else { "flutter" }
}

# --- Setup (установка зависимостей) ---
function Invoke-Setup {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  RiftLink — установка зависимостей" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""

    # 1. Python
    Write-Host "[1/7] Проверка Python..." -ForegroundColor Yellow
    $pyCmd = $null
    if (Get-Command python -ErrorAction SilentlyContinue) { $pyCmd = "python" }
    elseif (Get-Command py -ErrorAction SilentlyContinue) { $pyCmd = "py -3" }
    if (-not $pyCmd) {
        Write-Host "  Установка Python через winget..." -ForegroundColor Gray
        winget install Python.Python.3.12 --accept-package-agreements --accept-source-agreements 2>$null
        if ($LASTEXITCODE -eq 0) { Write-Host "  Python установлен. Перезапустите терминал." -ForegroundColor Green; return }
        Write-Host "[ОШИБКА] Установите Python 3.10+ с https://www.python.org/" -ForegroundColor Red
        return
    }
    if ($pyCmd -eq "py -3") { py -3 --version } else { python --version }
    Write-Host ""

    # 2. pip
    Write-Host "[2/7] pip-пакеты (pyserial, pytest, platformio)..." -ForegroundColor Yellow
    $reqPath = Join-Path $ScriptRoot "requirements-dev.txt"
    if ($pyCmd -eq "py -3") {
        py -3 -m pip install --upgrade pip -q 2>$null
        py -3 -m pip install -r $reqPath -q
    } else {
        python -m pip install --upgrade pip -q 2>$null
        python -m pip install -r $reqPath -q
    }
    if ($LASTEXITCODE -ne 0) { Write-Host "[ОШИБКА] pip install" -ForegroundColor Red; return }
    Write-Host "  OK" -ForegroundColor Green
    Write-Host ""

    # 3. PlatformIO
    Write-Host "[3/7] PlatformIO..." -ForegroundColor Yellow
    if (Get-Command pio -ErrorAction SilentlyContinue) { pio --version }
    elseif ($pyCmd -eq "py -3") { py -3 -m platformio --version }
    else { python -m platformio --version }
    if ($LASTEXITCODE -ne 0) { Write-Host "[ОШИБКА] pip install -U platformio" -ForegroundColor Red; return }
    Write-Host ""

    # 4. Java
    Write-Host "[4/7] Java..." -ForegroundColor Yellow
    if (-not (Get-Command java -ErrorAction SilentlyContinue)) {
        Write-Host "  Установка Java 17..." -ForegroundColor Gray
        winget install EclipseAdoptium.Temurin.17.JDK --accept-package-agreements --accept-source-agreements 2>$null
        Write-Host "  Перезапустите терминал." -ForegroundColor Gray
    } else { java -version 2>$null }
    Write-Host ""

    # 5. Flutter
    Write-Host "[5/7] Flutter..." -ForegroundColor Yellow
    $flutterPath = $Flutter
    if ($flutterPath -eq "flutter" -and -not (Get-Command flutter -ErrorAction SilentlyContinue)) {
        Write-Host "  Установка Flutter через winget..." -ForegroundColor Gray
        winget install Google.Flutter --accept-package-agreements --accept-source-agreements 2>$null
        if ($LASTEXITCODE -eq 0) { Write-Host "  Перезапустите терминал." -ForegroundColor Gray; return }
    }
    if ($flutterPath -ne "flutter") { & $flutterPath --version } else { flutter --version }
    Write-Host "  pub get (app)..." -ForegroundColor Gray
    Push-Location $AppDir
    try {
        if ($flutterPath -ne "flutter") { & $flutterPath pub get } else { flutter pub get }
    } finally {
        Pop-Location
    }
    Write-Host ""

    # 6. Android SDK
    Write-Host "[6/7] Android SDK..." -ForegroundColor Yellow
    if (-not (Test-Path $Adb)) {
        Write-Host "  Установка Android Studio..." -ForegroundColor Gray
        winget install Google.AndroidStudio --accept-package-agreements --accept-source-agreements 2>$null
        Write-Host "  Запустите Android Studio для настройки SDK." -ForegroundColor Gray
    } else { Write-Host "  OK: $env:ANDROID_HOME" -ForegroundColor Green }
    Write-Host ""

    Write-Host "[7/7] Готово!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Сборка: .\build.ps1 -V4" -ForegroundColor Cyan
    Write-Host "Прошивка: .\build.ps1 -V4 -Flash" -ForegroundColor Cyan
    Write-Host "APK: .\build.ps1 -App -InstallApk" -ForegroundColor Cyan
    Write-Host ""
}

# Сопоставление env для firmware
$EnvMap = @{
    "1" = @{ env = "heltec_v3";      name = "V3 (OLED)" }
    "2" = @{ env = "heltec_v3_paper"; name = "V3 Paper (e-ink)" }
    "3" = @{ env = "heltec_v4";       name = "V4 (OLED)" }
    "4" = @{ env = "faketec_v5";     name = "FakeTech V5 (nRF52)" }
}

function Get-SerialPorts {
    try {
        Push-Location $FirmwareDir
        $out = pio device list 2>&1
    } finally {
        Pop-Location
    }
    $ports = @()
    $lines = $out -split "`n"
    $i = 0
    while ($i -lt $lines.Count) {
        $line = $lines[$i].Trim()
        if ($line -match "^(COM\d+)$") {
            $port = $Matches[1]
            $desc = ""
            $hwid = ""
            $i++
            while ($i -lt $lines.Count -and $lines[$i] -notmatch "^(COM\d+)$" -and $lines[$i] -ne "---") {
                if ($lines[$i] -match "Description:\s*(.+)") { $desc = $Matches[1].Trim() }
                if ($lines[$i] -match "Hardware ID:\s*(.+)") { $hwid = $Matches[1].Trim() }
                $i++
            }
            if ($hwid -notmatch "BTHENUM") {
                $hint = "USB"
                if ($hwid -match "303A|VID_303A|ESP32|JTAG" -or $desc -match "JTAG|USB Serial|debug") {
                    $hint = "V4 (native USB)"
                } elseif ($hwid -match "CP210|10C4:EA60|Silicon Labs" -or $desc -match "CP210|Silicon Labs") {
                    $hint = "Paper (CP210x)"
                } elseif ($hwid -match "239A|VID_239A|Adafruit" -or $desc -match "Adafruit|последовательн") {
                    $hint = "FakeTech (nRF52)"
                }
                $ports += [PSCustomObject]@{ Port = $port; Desc = $desc; HwId = $hwid; Hint = $hint }
            }
            # $i уже на следующем COM — не увеличивать, чтобы его обработать
        } else {
            $i++
        }
    }
    return $ports
}

function Get-AdbDevices {
    if (-not (Test-Path $Adb)) { return @() }
    $out = & $Adb devices 2>&1
    $devs = @()
    foreach ($line in $out) {
        $line = $line.TrimEnd("`r").Trim()
        if ($line -match "^\s*(\S+)\s+(device|unauthorized)\s*$") {
            $id = $Matches[1].Trim()
            if ($id -notin @("List", "of", "attached")) {
                $devs += $id
            }
        }
    }
    return $devs
}

function Show-Menu {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  RiftLink — менеджер сборки" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  Прошивка (firmware):"
    Write-Host "    1. Сборка прошивки (выбор устройства)"
    Write-Host "    2. Прошивка (выбор порта + устройства)"
    Write-Host "    3. Сборка + прошивка"
    Write-Host "    4. Монитор порта"
    Write-Host ""
    Write-Host "  Приложение (APK):"
    Write-Host "    5. Сборка APK"
    Write-Host "    6. Установка APK на устройство (adb)"
    Write-Host "    7. Сборка + установка APK"
    Write-Host ""
    Write-Host "  Окружение:"
    Write-Host "    8. Setup — установка зависимостей (Python, PlatformIO, Flutter, Android)"
    Write-Host ""
    Write-Host "    0. Выход"
    Write-Host ""
}

function Invoke-BuildFirmware {
    param([string]$EnvName)
    Write-Host "[RiftLink] Сборка $EnvName..." -ForegroundColor Cyan
    try {
        Push-Location $FirmwareDir
        pio run -e $EnvName
        $ok = $LASTEXITCODE -eq 0
        if ($ok) {
            $outDir = Join-Path $FirmwareDir "out\$EnvName"
            New-Item -ItemType Directory -Force -Path $outDir | Out-Null
            $buildDir = Join-Path $FirmwareDir ".pio\build\$EnvName"
            if ($EnvName -eq "faketec_v5") {
                $hex = Join-Path $buildDir "firmware.hex"
                $zip = Join-Path $buildDir "firmware.zip"
                if (Test-Path $hex) { Copy-Item $hex (Join-Path $outDir "firmware.hex") -Force; Write-Host "[out] $EnvName/firmware.hex" -ForegroundColor Green }
                if (Test-Path $zip) { Copy-Item $zip (Join-Path $outDir "firmware.zip") -Force; Write-Host "[out] $EnvName/firmware.zip" -ForegroundColor Green }
            } else {
                $src = Join-Path $buildDir "firmware.bin"
                if (Test-Path $src) {
                    Copy-Item $src (Join-Path $outDir "firmware.bin") -Force
                    Write-Host "[out] $EnvName/firmware.bin" -ForegroundColor Green
                }
            }
        }
        return $ok
    } finally {
        Pop-Location
    }
}

function Invoke-FlashFirmware {
    param([string]$EnvName, [string]$UploadPort, [switch]$Erase)
    try {
        Push-Location $FirmwareDir
        if ($Erase) {
            Write-Host "[RiftLink] Очистка flash на $UploadPort..." -ForegroundColor Yellow
            pio run -e $EnvName -t erase --upload-port $UploadPort
        }
        Write-Host "[RiftLink] Прошивка $EnvName на $UploadPort..." -ForegroundColor Cyan
        pio run -e $EnvName -t upload --upload-port $UploadPort
        return $LASTEXITCODE -eq 0
    } finally {
        Pop-Location
    }
}

function Invoke-BuildApk {
    Write-Host "[RiftLink] Сборка APK..." -ForegroundColor Cyan
    try {
        Push-Location $AppDir
        & $Flutter pub get
        & $Flutter build apk --release --target-platform android-arm64
        $ok = $LASTEXITCODE -eq 0
        if ($ok) {
            $apk = Join-Path $AppDir "build\app\outputs\flutter-apk\app-release.apk"
            if (Test-Path $apk) {
                $size = [math]::Round((Get-Item $apk).Length / 1MB, 1)
                Write-Host "[out] $apk ($size MB)" -ForegroundColor Green
            }
        }
        return $ok
    } finally {
        Pop-Location
    }
}

function Invoke-InstallApk {
    param([string]$DeviceId = "")
    $apk = Join-Path $AppDir "build\app\outputs\flutter-apk\app-release.apk"
    if (-not (Test-Path $apk)) {
        Write-Host "[ОШИБКА] APK не найден. Сначала выполните сборку (п.5)." -ForegroundColor Red
        return $false
    }
    if (-not (Test-Path $Adb)) {
        Write-Host "[ОШИБКА] ADB не найден. ANDROID_HOME=$env:ANDROID_HOME" -ForegroundColor Red
        return $false
    }
    $devs = @(Get-AdbDevices)
    if ($devs.Count -eq 0) {
        Write-Host "[ОШИБКА] Нет подключённых устройств. adb devices" -ForegroundColor Red
        return $false
    }
    $target = $null
    $rawId = $DeviceId.Trim()
    if ($rawId) {
        $idx = -1
        if ([int]::TryParse($rawId, [ref]$idx) -and $idx -ge 1 -and $idx -le $devs.Count) {
            $target = $devs[$idx - 1]
            Write-Host "  Устройство $idx : $target" -ForegroundColor Gray
        } elseif ($devs -contains $rawId) {
            $target = $rawId
        } else {
            Write-Host "[ОШИБКА] -DeviceId '$rawId': введите номер (1-$($devs.Count)) или ID устройства" -ForegroundColor Red
            Write-Host "  adb devices:" -ForegroundColor Gray
            & $Adb devices 2>&1 | ForEach-Object { Write-Host "    $_" }
            return $false
        }
    } else {
        if ($devs.Count -eq 1) {
            $target = $devs[0]
            Write-Host "  Устройство: $target" -ForegroundColor Gray
        } else {
            Write-Host ""
            Write-Host "Подключённые устройства (adb devices):"
            & $Adb devices 2>&1 | ForEach-Object { Write-Host "  $_" }
            Write-Host ""
            for ($i = 0; $i -lt $devs.Count; $i++) {
                Write-Host "  $($i+1). $($devs[$i])"
            }
            Write-Host ""
            $sel = Read-Host "Введите номер устройства (1-$($devs.Count))"
            $idx = -1
            if ([int]::TryParse($sel.Trim(), [ref]$idx)) { $idx -= 1 } else { $idx = -1 }
            if ($idx -lt 0 -or $idx -ge $devs.Count) {
                Write-Host "[ОШИБКА] Введите число от 1 до $($devs.Count)" -ForegroundColor Red
                return $false
            }
            $target = $devs[$idx]
        }
    }
    if ($target) {
        Write-Host "[RiftLink] Установка APK на $target..." -ForegroundColor Cyan
        $null = & $Adb -s $target get-state 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ОШИБКА] Устройство $target недоступно. adb devices:" -ForegroundColor Red
            & $Adb devices 2>&1 | ForEach-Object { Write-Host "  $_" }
            return $false
        }
        & $Adb -s $target install -r $apk
        return $LASTEXITCODE -eq 0
    }
    return $false
}

# Режим CLI (без меню) — сохраняем текущую папку, возвращаемся при выходе
try {
# Режим CLI (без меню)
if ($Setup) {
    Invoke-Setup
    exit 0
}
# Монитор порта (просмотр вывода устройства)
if ($Monitor -and -not $Flash -and -not $App -and -not $InstallApk -and -not $All -and $BuildEnv -eq "") {
    $ports = @(Get-SerialPorts)
    if ($ports.Count -eq 0) {
        Write-Host "[ОШИБКА] Нет USB-портов. Подключите устройство." -ForegroundColor Red
        exit 1
    }
    $monitorPort = $Port
    if (-not $monitorPort) {
        if ($ports.Count -eq 1) { $monitorPort = $ports[0].Port }
        else {
            Write-Host "Порты:"
            for ($i = 0; $i -lt $ports.Count; $i++) {
                Write-Host "  $($i+1). $($ports[$i].Port) - $($ports[$i].Hint)"
            }
            $sel = Read-Host "Выберите порт (1-$($ports.Count))"
            $idx = [int]$sel - 1
            if ($idx -ge 0 -and $idx -lt $ports.Count) { $monitorPort = $ports[$idx].Port }
        }
    }
    if ($monitorPort) {
        Write-Host "[RiftLink] Монитор $monitorPort (Ctrl+C — выход)" -ForegroundColor Cyan
        try {
            Push-Location $FirmwareDir
            pio device monitor --port $monitorPort
        } finally {
            Pop-Location
        }
    }
    exit $LASTEXITCODE
}
if ($Flash -or $App -or $InstallApk -or $All -or $FakeTech -or $BuildEnv -ne "") {
    $envChoice = $BuildEnv
    if ($FakeTech) { $envChoice = "faketec_v5" }
    elseif ($V4) { $envChoice = "heltec_v4" }
    elseif ($V3Paper) { $envChoice = "heltec_v3_paper" }
    elseif ($V3 -or $envChoice -eq "") { $envChoice = "heltec_v3" }
    if ($Ota -and $envChoice -ne "faketec_v5") {
        $envChoice = switch ($envChoice) {
            "heltec_v4" { "heltec_v4_ota" }
            "heltec_v3_paper" { "heltec_v3_paper_ota" }
            default { "heltec_v3_ota" }
        }
    }
    if ($App -and $InstallApk) {
        if (Invoke-BuildApk) { Invoke-InstallApk -DeviceId $DeviceId | Out-Null }
        exit $LASTEXITCODE
    }
    if ($App -and -not $InstallApk) {
        Invoke-BuildApk | Out-Null
        exit $LASTEXITCODE
    }
    if ($InstallApk -and -not $App) {
        Invoke-InstallApk -DeviceId $DeviceId | Out-Null
        exit $LASTEXITCODE
    }
    Set-Location $FirmwareDir
    if ($All) {
        $allEnvs = @("heltec_v3", "heltec_v3_paper", "heltec_v4", "faketec_v5")
        foreach ($e in $allEnvs) {
            Invoke-BuildFirmware -EnvName $e
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        exit 0
    }
    if ($Flash) {
        $uploadPort = $Port
            if (-not $uploadPort) {
            $ports = @(Get-SerialPorts)
            if ($ports.Count -eq 0) {
                Write-Host "[ОШИБКА] Нет подключённых устройств." -ForegroundColor Red
                exit 1
            }
            $preferPaper = $envChoice -match "paper"
            $preferV4 = $envChoice -match "v4"
            $preferFakeTech = $envChoice -match "faketec"
            if ($ports.Count -eq 1) {
                $uploadPort = $ports[0].Port
            } else {
                $sorted = @($ports | ForEach-Object {
                    $s = 0
                    if ($preferFakeTech -and $_.Hint -match "FakeTech") { $s = 10 }
                    elseif ($preferPaper -and $_.Hint -match "Paper") { $s = 10 }
                    elseif ($preferV4 -and $_.Hint -match "V4") { $s = 10 }
                    $comNum = [int]($_.Port -replace 'COM', '')
                    [PSCustomObject]@{ Obj = $_; Score = $s; ComNum = $comNum }
                } | Sort-Object { -$_.Score }, { $_.ComNum } | ForEach-Object { $_.Obj })
                $n = $sorted.Count
                Write-Host "Порты (автоопределение):"
                for ($i = 0; $i -lt $n; $i++) {
                    $p = $sorted[$i]
                    $m = ""
                    if ($preferFakeTech -and $p.Hint -match "FakeTech") { $m = " <- FakeTech" }
                    elseif ($preferPaper -and $p.Hint -match "Paper") { $m = " <- Paper" }
                    elseif ($preferV4 -and $p.Hint -match "V4") { $m = " <- V4" }
                    Write-Host "  $($i+1). $($p.Port) - $($p.Hint)$m"
                }
                $sel = Read-Host "Выберите порт (1-$n)"
                $idx = [int]$sel - 1
                if ($idx -ge 0 -and $idx -lt $n) { $uploadPort = $sorted[$idx].Port }
            }
        }
        if ($uploadPort) {
            if ($envChoice -eq "faketec_v5") {
                Write-Host "[FakeTech] Двойной клик RST на NiceNano для DFU, затем прошивка..." -ForegroundColor Yellow
            }
            Invoke-FlashFirmware -EnvName $envChoice -UploadPort $uploadPort -Erase:$Erase
        }
    }
    else {
        Invoke-BuildFirmware -EnvName $envChoice
    }
    if ($Monitor) { pio device monitor }
    exit $LASTEXITCODE
}

# Интерактивное меню
while ($true) {
    Show-Menu
    $choice = Read-Host "Выберите действие"
    switch ($choice) {
        "1" {
            Write-Host ""
            Write-Host "  Выберите прошивку:"
            foreach ($k in $EnvMap.Keys | Sort-Object) {
                Write-Host "    $k. $($EnvMap[$k].name)"
            }
            $e = Read-Host "Номер"
            if ($EnvMap.ContainsKey($e)) {
                Invoke-BuildFirmware -EnvName $EnvMap[$e].env
            }
        }
        "2" {
            Write-Host ""
            Write-Host "  Выберите прошивку:"
            foreach ($k in $EnvMap.Keys | Sort-Object) {
                Write-Host "    $k. $($EnvMap[$k].name)"
            }
            $e = Read-Host "Номер"
            if (-not $EnvMap.ContainsKey($e)) { break }
            $envName = $EnvMap[$e].env
            $ports = @(Get-SerialPorts)
            if ($ports.Count -eq 0) {
                Write-Host "[ОШИБКА] Нет USB-портов (COM). Bluetooth не подходит." -ForegroundColor Red
                break
            }
            $port = $null
            $preferFakeTech = $envName -match "faketec"
            $preferPaper = $envName -match "paper"
            $preferV4 = $envName -match "v4"
            if ($ports.Count -eq 1) {
                $port = $ports[0].Port
                Write-Host ""
                Write-Host "  Порт: $($ports[0].Port) [$($ports[0].Hint)]" -ForegroundColor Gray
            } else {
                $sorted = @($ports | ForEach-Object {
                    $score = 0
                    if ($preferFakeTech -and $_.Hint -match "FakeTech") { $score = 10 }
                    elseif ($preferPaper -and $_.Hint -match "Paper") { $score = 10 }
                    elseif ($preferV4 -and $_.Hint -match "V4") { $score = 10 }
                    $comNum = [int]($_.Port -replace 'COM', '')
                    [PSCustomObject]@{ Obj = $_; Score = $score; ComNum = $comNum }
                } | Sort-Object { -$_.Score }, { $_.ComNum } | ForEach-Object { $_.Obj })
                $n = $sorted.Count
                Write-Host ""
                Write-Host "  Подключённые порты (автоопределение по USB):"
                for ($i = 0; $i -lt $n; $i++) {
                    $p = $sorted[$i]
                    $mark = ""
                    if ($preferFakeTech -and $p.Hint -match "FakeTech") { $mark = " <- рекомендуется для FakeTech" }
                    elseif ($preferPaper -and $p.Hint -match "Paper") { $mark = " <- рекомендуется для Paper" }
                    elseif ($preferV4 -and $p.Hint -match "V4") { $mark = " <- рекомендуется для V4" }
                    Write-Host "    $($i+1). $($p.Port) - $($p.Hint)$mark"
                }
                $pSel = Read-Host "Порт (1-$n)"
                $pIdx = [int]$pSel - 1
                if ($pIdx -ge 0 -and $pIdx -lt $n) { $port = $sorted[$pIdx].Port }
            }
            if ($port) {
                if ($envName -eq "faketec_v5") {
                    Write-Host "  [FakeTech] Двойной клик RST на NiceNano для DFU перед прошивкой." -ForegroundColor Yellow
                }
                $yn = Read-Host "Очистить flash перед прошивкой? (y/n)"
                $doErase = $yn -match '^[yY]'
                Invoke-FlashFirmware -EnvName $envName -UploadPort $port -Erase:$doErase
            }
        }
        "3" {
            Write-Host ""
            Write-Host "  Выберите прошивку:"
            foreach ($k in $EnvMap.Keys | Sort-Object) {
                Write-Host "    $k. $($EnvMap[$k].name)"
            }
            $e = Read-Host "Номер"
            if (-not $EnvMap.ContainsKey($e)) { break }
            $envName = $EnvMap[$e].env
            if (-not (Invoke-BuildFirmware -EnvName $envName)) { break }
            $ports = @(Get-SerialPorts)
            if ($ports.Count -eq 0) {
                Write-Host "[ОШИБКА] Нет USB-портов (COM). Bluetooth не подходит." -ForegroundColor Red
                break
            }
            $port = $null
            $preferFakeTech = $envName -match "faketec"
            $preferPaper = $envName -match "paper"
            $preferV4 = $envName -match "v4"
            if ($ports.Count -eq 1) {
                $port = $ports[0].Port
                Write-Host ""
                Write-Host "  Порт: $($ports[0].Port) [$($ports[0].Hint)]" -ForegroundColor Gray
            } else {
                $sorted = @($ports | ForEach-Object {
                    $score = 0
                    if ($preferFakeTech -and $_.Hint -match "FakeTech") { $score = 10 }
                    elseif ($preferPaper -and $_.Hint -match "Paper") { $score = 10 }
                    elseif ($preferV4 -and $_.Hint -match "V4") { $score = 10 }
                    $comNum = [int]($_.Port -replace 'COM', '')
                    [PSCustomObject]@{ Obj = $_; Score = $score; ComNum = $comNum }
                } | Sort-Object { -$_.Score }, { $_.ComNum } | ForEach-Object { $_.Obj })
                $n = $sorted.Count
                Write-Host ""
                Write-Host "  Подключённые порты (автоопределение по USB):"
                for ($i = 0; $i -lt $n; $i++) {
                    $p = $sorted[$i]
                    $mark = ""
                    if ($preferFakeTech -and $p.Hint -match "FakeTech") { $mark = " <- рекомендуется для FakeTech" }
                    elseif ($preferPaper -and $p.Hint -match "Paper") { $mark = " <- рекомендуется для Paper" }
                    elseif ($preferV4 -and $p.Hint -match "V4") { $mark = " <- рекомендуется для V4" }
                    Write-Host "    $($i+1). $($p.Port) - $($p.Hint)$mark"
                }
                $pSel = Read-Host "Порт (1-$n)"
                $pIdx = [int]$pSel - 1
                if ($pIdx -ge 0 -and $pIdx -lt $n) { $port = $sorted[$pIdx].Port }
            }
            if ($port) {
                if ($envName -eq "faketec_v5") {
                    Write-Host "  [FakeTech] Двойной клик RST на NiceNano для DFU перед прошивкой." -ForegroundColor Yellow
                }
                $yn = Read-Host "Очистить flash перед прошивкой? (y/n)"
                $doErase = $yn -match '^[yY]'
                Invoke-FlashFirmware -EnvName $envName -UploadPort $port -Erase:$doErase
            }
        }
        "4" {
            $ports = @(Get-SerialPorts)
            if ($ports.Count -eq 0) {
                Write-Host "[ОШИБКА] Нет портов." -ForegroundColor Red
                break
            }
            $port = $ports[0].Port
            if ($ports.Count -gt 1) {
                for ($i = 0; $i -lt $ports.Count; $i++) {
                    Write-Host "  $($i+1). $($ports[$i].Port) - $($ports[$i].Hint)"
                }
                $pSel = Read-Host "Порт"
                $pIdx = [int]$pSel - 1
                if ($pIdx -ge 0 -and $pIdx -lt $ports.Count) { $port = $ports[$pIdx].Port }
            }
            try {
                Push-Location $FirmwareDir
                pio device monitor --port $port
            } finally {
                Pop-Location
            }
        }
        "5" {
            Invoke-BuildApk | Out-Null
        }
        "6" {
            Invoke-InstallApk | Out-Null
        }
        "7" {
            if (Invoke-BuildApk) { Invoke-InstallApk | Out-Null }
        }
        "8" { Invoke-Setup }
        "0" { exit 0 }
        default { Write-Host "Неверный выбор." -ForegroundColor Yellow }
    }
    Write-Host ""
    Read-Host "Нажмите Enter для продолжения"
}
} finally {
    Set-Location $StartDir
}
