#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PID_DIR="${ROOT_DIR}/.ghostfs_pids"
mkdir -p "${PID_DIR}"

BACKEND_PORT="${GHOSTFS_BACKEND_PORT:-8000}"
FRONTEND_PORT="${GHOSTFS_FRONTEND_PORT:-5173}"

if [ -f "${PID_DIR}/backend.pid" ] && kill -0 "$(cat "${PID_DIR}/backend.pid")" 2>/dev/null; then
  echo "Backend already running (PID $(cat "${PID_DIR}/backend.pid"))"
else
  "${ROOT_DIR}/backend/.venv/bin/uvicorn" app.main:app \
    --app-dir "${ROOT_DIR}/backend" \
    --host 127.0.0.1 --port "${BACKEND_PORT}" \
    >"${PID_DIR}/backend.log" 2>&1 &
  echo $! >"${PID_DIR}/backend.pid"
fi

if [ -f "${PID_DIR}/frontend.pid" ] && kill -0 "$(cat "${PID_DIR}/frontend.pid")" 2>/dev/null; then
  echo "Frontend already running (PID $(cat "${PID_DIR}/frontend.pid"))"
else
  npm --prefix "${ROOT_DIR}/frontend" run dev -- --host 127.0.0.1 --port "${FRONTEND_PORT}" \
    >"${PID_DIR}/frontend.log" 2>&1 &
  echo $! >"${PID_DIR}/frontend.pid"
fi

sleep 2
for name in backend frontend; do
  pid="$(cat "${PID_DIR}/${name}.pid")"
  if ! kill -0 "${pid}" 2>/dev/null; then
    echo "ERROR: ${name} failed to start. See ${PID_DIR}/${name}.log" >&2
    exit 1
  fi
done
echo "[GhostFS start] Backend: http://127.0.0.1:${BACKEND_PORT} | Frontend: http://127.0.0.1:${FRONTEND_PORT}"
