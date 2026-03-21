#!/bin/bash
# RiftLink — обновление из репозитория (перезапись всех локальных изменений)
# Использование: ./update.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo ""
echo "========================================"
echo "  RiftLink — обновление из репозитория"
echo "  (перезапись всех локальных изменений)"
echo "========================================"
echo ""

if [[ ! -d ".git" ]]; then
    echo "[ОШИБКА] Не git-репозиторий."
    exit 1
fi

if ! command -v git &>/dev/null; then
    echo "[ОШИБКА] Git не найден. Установите: https://git-scm.com/"
    exit 1
fi

echo "Получение изменений с удалённого репозитория..."
git fetch origin

BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo main)"
echo "Сброс на origin/$BRANCH (перезапись локальных изменений)..."
git reset --hard "origin/$BRANCH"

echo "Удаление неотслеживаемых файлов..."
git clean -fd

echo "Восстановление прав на скрипты..."
chmod +x "$0" build.sh install.sh 2>/dev/null || true

echo ""
echo "========================================"
echo "  Готово! Репозиторий обновлён."
echo "========================================"
echo ""
