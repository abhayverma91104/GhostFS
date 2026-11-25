#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "[GhostFS test] 1/3 C++17 CTest suite..."
ctest --test-dir "${ROOT_DIR}/engine/build" --output-on-failure

echo "[GhostFS test] 2/3 FastAPI Pytest suite..."
"${ROOT_DIR}/backend/.venv/bin/pytest" "${ROOT_DIR}/backend/tests" -v

echo "[GhostFS test] 3/3 Frontend production build..."
npm --prefix "${ROOT_DIR}/frontend" run build

if [ "${RUN_E2E:-0}" = "1" ]; then
  echo "[GhostFS test] Optional Playwright E2E suite..."
  npx --prefix "${ROOT_DIR}/frontend" playwright test --config "${ROOT_DIR}/frontend/playwright.config.ts"
fi

echo "[GhostFS test] All mandatory checks passed."
