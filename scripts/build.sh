#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "[GhostFS build] Configuring & building C++17 engine..."
cmake -S "${ROOT_DIR}/engine" -B "${ROOT_DIR}/engine/build"
cmake --build "${ROOT_DIR}/engine/build" -j4

echo "[GhostFS build] Building React frontend..."
npm --prefix "${ROOT_DIR}/frontend" run build
echo "[GhostFS build] Complete."
