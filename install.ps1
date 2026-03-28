# RiftLink — установка «в один клик»: git clone + build.ps1 -Setup
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
    Write-Host "  Папка $Dest уже существует. Обновление (перезапись локальных изменений)..."
    Push-Location $Dest
    git fetch origin
    $branch = git rev-parse --abbrev-ref HEAD 2>$null
    if (-not $branch) { $branch = "main" }
    git reset --hard "origin/$branch"
    git clean -fd
    Pop-Location
} else {
    git clone $Repo $Dest
}
Write-Host ""

# 3. Запуск setup (build.ps1 -Setup)
Write-Host "[3/3] Установка зависимостей (build.ps1 -Setup)..." -ForegroundColor Yellow
$buildPs1 = Join-Path $Dest "build.ps1"
if (Test-Path $buildPs1) {
    Push-Location $Dest
    & $buildPs1 -Setup
    Pop-Location
} else {
    Write-Host "[ОШИБКА] build.ps1 не найден в $Dest" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Готово! cd $Dest; .\build.ps1" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Прошивки:" -ForegroundColor Gray
Write-Host "    Heltec V3/V4:  .\build.ps1 -V4 -Flash" -ForegroundColor Gray
Write-Host ""
