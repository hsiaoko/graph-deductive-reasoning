#!/usr/bin/env bash
set -euo pipefail

: "${JEV_MODEL_PATH:?Set JEV_MODEL_PATH to the local model directory}"

HOST="${JEV_HOST:-127.0.0.1}"
PORT="${JEV_PORT:-30000}"
MODEL_NAME="${JEV_MODEL_NAME:-local-model}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VENV_BIN="${JEV_VENV_BIN:-$ROOT_DIR/third_party/llm2jev/.venv/bin}"
export PATH="$VENV_BIN:$PATH"

if ! command -v llm2jev-serve >/dev/null 2>&1; then
  echo "llm2jev-serve is unavailable. Run: uv sync --extra sglang in third_party/llm2jev and activate .venv" >&2
  exit 1
fi

exec llm2jev-serve \
  --backend sglang \
  --model-path "$JEV_MODEL_PATH" \
  --served-model-name "$MODEL_NAME" \
  --host "$HOST" \
  --port "$PORT"
