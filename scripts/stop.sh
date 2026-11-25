#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PID_DIR="${ROOT_DIR}/.ghostfs_pids"

for name in frontend backend; do
  pid_file="${PID_DIR}/${name}.pid"
  if [ -f "${pid_file}" ]; then
    pid="$(cat "${pid_file}")"
    if kill -0 "${pid}" 2>/dev/null; then
      pkill -TERM -P "${pid}" 2>/dev/null || true
      kill "${pid}" 2>/dev/null || true
      for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.2
      done
      echo "[GhostFS stop] Stopped ${name} (PID ${pid})"
    fi
    rm -f "${pid_file}"
  fi
done
