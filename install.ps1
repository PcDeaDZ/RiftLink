# RiftLink — установка «в один клик»: git clone + setup_env
# Использование: irm https://raw.githubusercontent.com/PcDeaDZ/RiftLink/master/install.ps1 | iex
# Или: Invoke-Expression (Invoke-WebRequest -Uri "https://raw.../install.ps1" -UseBasicParsing).Content
#
# Переменные: $env:RIFTLINK_REPO (URL репозитория), $env:RIFTLINK_DIR (папка, по умолчанию $HOME\riftlink)

$ErrorActionPreference = "Stop"
$Repo = if ($env:RIFTLINK_REPO) { $env:RIFTLINK_REPO } else { "https://github.com/PcDeaDZ/RiftLink.git" }
$Dest = if ($env:RIFTLINK_DIR) { $env:RIFTLINK_DIR } else { Join-Path $env:USERPROFILE "riftlink" }

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  RiftLink — установка" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Репозиторий: $Repo"
Write-Host "  Папка: $Dest"
Write-Host ""

# 1. Проверка/установка git
Write-Host "[1/3] Проверка Git..." -ForegroundColor Yellow
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    Write-Host "  Установка Git через winget..."
    winget install Git.Git --accept-package-agreements --accept-source-agreements
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
    Write-Host "  Перезапустите терминал и запустите install.ps1 снова."
    exit 0
}
Write-Host "  Git: $(git --version)"
Write-Host ""

# 2. Клонирование
Write-Host "[2/3] Клонирование репозитория..." -ForegroundColor Yellow
if (Test-Path (Join-Path $Dest ".git")) {
    Write-Host "  Папка $Dest уже существует. Обновление..."
    Push-Location $Dest
    git pull
    Pop-Location
} else {
    git clone $Repo $Dest
}
Write-Host ""

# 3. Запуск setup_env.bat
Write-Host "[3/3] Установка зависимостей (setup_env.bat)..." -ForegroundColor Yellow
$setupBat = Join-Path $Dest "setup_env.bat"
if (Test-Path $setupBat) {
    Push-Location $Dest
    cmd /c "setup_env.bat"
    Pop-Location
} else {
    Write-Host "[ОШИБКА] setup_env.bat не найден в $Dest" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Готово! cd $Dest; .\build.ps1" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Прошивки:" -ForegroundColor Gray
Write-Host "    Heltec V3/V4:  .\build.ps1 -V4 -Flash" -ForegroundColor Gray
Write-Host "    FakeTech V5:   .\build.ps1 -FakeTech        # сборка" -ForegroundColor Gray
Write-Host "                  .\build.ps1 -FakeTech -Flash  # сборка + прошивка" -ForegroundColor Gray
Write-Host "                  Для erase (-Erase): winget install NordicSemiconductor.JLink + J-Link probe по SWD" -ForegroundColor Gray
Write-Host ""
