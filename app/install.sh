#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# 1. venv
python3 -m venv .venv
. .venv/bin/activate

# 2. зависимости
pip install --upgrade pip
pip install -r requirements.txt

echo "OK. Запуск: ./run.sh"
