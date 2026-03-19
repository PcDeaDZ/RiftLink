#!/bin/bash
# RiftLink — установка «в один клик»: git clone + setup_env
# Использование: curl -fsSL https://raw.githubusercontent.com/PcDeaDZ/RiftLink/master/install.sh | bash
# Или: wget -qO- https://raw.../install.sh | bash
#
# Переменные: RIFTLINK_REPO (URL репозитория), RIFTLINK_DIR (папка для клона, по умолчанию ~/riftlink)

set -e

REPO="${RIFTLINK_REPO:-https://github.com/PcDeaDZ/RiftLink.git}"
DEST="${RIFTLINK_DIR:-$HOME/riftlink}"

echo ""
echo "========================================"
echo "  RiftLink — установка"
echo "========================================"
echo ""
echo "  Репозиторий: $REPO"
echo "  Папка: $DEST"
echo ""

# 1. Проверка/установка git
if ! command -v git &>/dev/null; then
    echo "[1/3] Установка Git..."
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq && sudo apt-get install -y git
    elif command -v brew &>/dev/null; then
        brew install git
    elif command -v dnf &>/dev/null; then
        sudo dnf install -y git
    else
        echo "[ОШИБКА] Git не найден. Установите: https://git-scm.com/"
        exit 1
    fi
else
    echo "[1/3] Git: $(git --version)"
fi
echo ""

# 2. Клонирование
echo "[2/3] Клонирование репозитория..."
if [[ -d "$DEST/.git" ]]; then
    echo "  Папка $DEST уже существует (git). Обновление..."
    (cd "$DEST" && git pull)
else
    git clone "$REPO" "$DEST"
fi
echo ""

# 3. Запуск setup_env
echo "[3/3] Установка зависимостей (setup_env.sh)..."
cd "$DEST"
if [[ -f setup_env.sh ]]; then
    chmod +x setup_env.sh
    ./setup_env.sh
else
    echo "[ОШИБКА] setup_env.sh не найден в $DEST"
    exit 1
fi

echo ""
echo "========================================"
echo "  Готово! cd $DEST && ./build.sh"
echo "========================================"
echo ""
