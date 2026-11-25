#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "[GhostFS demo] Running complete investigation demonstration..."
"${ROOT_DIR}/backend/.venv/bin/python" "${ROOT_DIR}/backend/scripts/run_complete_demo.py"
