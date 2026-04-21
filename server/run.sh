#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
. .venv/bin/activate

# если нужна авторизация агентов:
# export AGENT_TOKEN="s3cret"

exec uvicorn main:app \
    --host 0.0.0.0 \
    --port 8000 \
    --proxy-headers \
    --no-access-log
