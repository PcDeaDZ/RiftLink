@echo off
chcp 65001 >nul
setlocal
echo.
echo ========================================
echo   RiftLink — обновление из репозитория
echo   (перезапись всех локальных изменений)
echo ========================================
echo.

cd /d "%~dp0"

if not exist ".git" (
    echo [ОШИБКА] Не git-репозиторий.
    pause
    exit /b 1
)

where git >nul 2>nul
if %errorlevel% neq 0 (
    echo [ОШИБКА] Git не найден. Установите: winget install Git.Git
    pause
    exit /b 1
)

echo Получение изменений с удалённого репозитория...
git fetch origin
if %errorlevel% neq 0 (
    echo [ОШИБКА] git fetch не удался.
    pause
    exit /b 1
)

for /f "tokens=*" %%b in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "BRANCH=%%b"
if not defined BRANCH set "BRANCH=main"

echo Сброс на origin/%BRANCH% (перезапись локальных изменений)...
git reset --hard origin/%BRANCH%
if %errorlevel% neq 0 (
    echo [ОШИБКА] git reset не удался.
    pause
    exit /b 1
)

echo Удаление неотслеживаемых файлов...
git clean -fd

echo.
echo ========================================
echo   Готово! Репозиторий обновлён.
echo ========================================
echo.
pause
