#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODE="${1:-combined}"

case "${MODE}" in
  asan)
    FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
    BUILD_DIR="${ROOT_DIR}/engine/build-asan"
    ;;
  ubsan)
    FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g"
    BUILD_DIR="${ROOT_DIR}/engine/build-ubsan"
    ;;
  combined)
    FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    BUILD_DIR="${ROOT_DIR}/engine/build-san"
    ;;
  *)
    echo "Usage: $0 [asan|ubsan|combined]" >&2
    exit 1
    ;;
esac

echo "[GhostFS sanitize] Mode=${MODE} BuildDir=${BUILD_DIR}"
cmake -S "${ROOT_DIR}/engine" -B "${BUILD_DIR}" \
  -DCMAKE_CXX_FLAGS="${FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="${FLAGS}"
cmake --build "${BUILD_DIR}" -j4
ctest --test-dir "${BUILD_DIR}" --output-on-failure
