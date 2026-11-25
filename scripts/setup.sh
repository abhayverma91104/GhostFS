#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "[GhostFS setup] Root: ${ROOT_DIR}"
for tool in cmake python3 npm; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "ERROR: Required tool '${tool}' not found in PATH." >&2
    exit 1
  fi
done

if [ ! -d "${ROOT_DIR}/backend/.venv" ]; then
  python3 -m venv "${ROOT_DIR}/backend/.venv"
fi
"${ROOT_DIR}/backend/.venv/bin/pip" install --upgrade pip >/dev/null
"${ROOT_DIR}/backend/.venv/bin/pip" install -r "${ROOT_DIR}/backend/requirements.txt"

if [ -f "${ROOT_DIR}/frontend/package-lock.json" ]; then
  (cd "${ROOT_DIR}/frontend" && npm ci)
else
  (cd "${ROOT_DIR}/frontend" && npm install)
fi

echo "[GhostFS setup] Complete."
