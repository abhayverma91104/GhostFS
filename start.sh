#!/usr/bin/env bash
set -euo pipefail

# Resolve project root dynamically regardless of current working directory
SOURCE_PATH="${BASH_SOURCE[0]}"
while [ -L "${SOURCE_PATH}" ]; do
  DIR="$(cd -P "$(dirname "${SOURCE_PATH}")" && pwd)"
  SOURCE_PATH="$(readlink "${SOURCE_PATH}")"
  case "${SOURCE_PATH}" in
    /*) ;;
    *) SOURCE_PATH="${DIR}/${SOURCE_PATH}" ;;
  esac
done
ROOT_DIR="$(cd -P "$(dirname "${SOURCE_PATH}")" && pwd)"

PID_DIR="${ROOT_DIR}/.ghostfs_pids"
STATE_DIR="${PID_DIR}/.state"
BACKEND_PORT="${GHOSTFS_BACKEND_PORT:-8000}"
FRONTEND_PORT="${GHOSTFS_FRONTEND_PORT:-5173}"

# Disable ANSI colors when not writing to a TTY, NO_COLOR is set, or TERM=dumb
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != "dumb" ]; then
  C_RESET=$'\033[0m'
  C_BOLD=$'\033[1m'
  C_CYAN=$'\033[36m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_RED=$'\033[31m'
  C_DIM=$'\033[2m'
else
  C_RESET=""
  C_BOLD=""
  C_CYAN=""
  C_GREEN=""
  C_YELLOW=""
  C_RED=""
  C_DIM=""
fi

detect_platform() {
  local raw_os raw_arch
  raw_os="$(uname -s)"
  raw_arch="$(uname -m)"
  case "${raw_os}" in
    Darwin) OS_NAME="macOS" ;;
    Linux)  OS_NAME="Linux" ;;
    *)      OS_NAME="${raw_os}" ;;
  esac
  ARCH_NAME="${raw_arch}"
}

print_banner() {
  printf "%s" "${C_CYAN}${C_BOLD}"
  cat <<'EOF'
   ____ _               _   _____ ____
  / ___| |__   ___  ___| |_|  ___/ ___|
 | |  _| '_ \ / _ \/ __| __| |_  \___ \
 | |_| | | | | (_) \__ \ |_|  _|  ___) |
  \____|_| |_|\___/|___/\__|_|   |____/
EOF
  printf "%s\n" "${C_RESET}${C_DIM}Digital Forensics & Custom File System Recovery Workstation (GFS1)${C_RESET}"
  printf "Platform : %s (%s)\n" "${OS_NAME}" "${ARCH_NAME}"
  printf "Root     : %s\n\n" "${ROOT_DIR}"
}

log_ok()   { printf "%s[OK]%s   %s\n" "${C_GREEN}" "${C_RESET}" "$1"; }
log_info() { printf "%s[INFO]%s %s\n" "${C_CYAN}" "${C_RESET}" "$1"; }
log_warn() { printf "%s[WARN]%s %s\n" "${C_YELLOW}" "${C_RESET}" "$1"; }
log_err()  { printf "%s[ERR]%s  %s\n" "${C_RED}" "${C_RESET}" "$1" >&2; }

check_prerequisites() {
  local missing=0
  local cxx_found=""

  for compiler in clang++ g++ c++; do
    if command -v "${compiler}" >/dev/null 2>&1; then
      cxx_found="${compiler}"
      break
    fi
  done
  if [ -z "${cxx_found}" ]; then
    log_err "Missing C++17 compiler (clang++, g++, or c++)."
    missing=1
  fi

  for tool in cmake python3 node npm; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
      log_err "Missing required tool: ${tool}"
      missing=1
    fi
  done

  if [ "${missing}" -ne 0 ]; then
    printf "\n%sActionable install instructions:%s\n" "${C_YELLOW}" "${C_RESET}" >&2
    if [ "${OS_NAME}" = "macOS" ]; then
      printf "  1. xcode-select --install\n" >&2
      printf "  2. brew install cmake python node\n" >&2
    else
      printf "  Ubuntu/Debian: sudo apt-get update && sudo apt-get install -y build-essential cmake python3 python3-venv nodejs npm\n" >&2
    fi
    exit 1
  fi

  local py_ver node_ver
  py_ver="$(python3 --version 2>&1)"
  node_ver="$(node --version 2>&1)"
  log_ok "Dependencies verified (${cxx_found}, cmake, ${py_ver}, node ${node_ver})"
}

file_sha256() {
  local target="$1"
  if [ ! -f "${target}" ]; then
    printf "none"
    return
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${target}" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${target}" | awk '{print $1}'
  else
    python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "${target}"
  fi
}

prepare_runtime_dirs() {
  mkdir -p \
    "${ROOT_DIR}/disk_images/sandbox" \
    "${ROOT_DIR}/reports/recovered" \
    "${ROOT_DIR}/reports/generated" \
    "${STATE_DIR}"
}

ensure_setup() {
  prepare_runtime_dirs
  local req_hash pkg_hash prev_req prev_pkg
  req_hash="$(file_sha256 "${ROOT_DIR}/backend/requirements.txt")"
  if [ -f "${ROOT_DIR}/frontend/package-lock.json" ]; then
    pkg_hash="$(file_sha256 "${ROOT_DIR}/frontend/package-lock.json")"
  else
    pkg_hash="$(file_sha256 "${ROOT_DIR}/frontend/package.json")"
  fi

  prev_req=""
  prev_pkg=""
  [ -f "${STATE_DIR}/req.sha256" ] && prev_req="$(cat "${STATE_DIR}/req.sha256")"
  [ -f "${STATE_DIR}/pkg.sha256" ] && prev_pkg="$(cat "${STATE_DIR}/pkg.sha256")"

  local need_py=0 need_npm=0
  if [ ! -x "${ROOT_DIR}/backend/.venv/bin/uvicorn" ] || [ "${req_hash}" != "${prev_req}" ]; then
    need_py=1
  fi
  if [ ! -d "${ROOT_DIR}/frontend/node_modules" ] || [ "${pkg_hash}" != "${prev_pkg}" ]; then
    need_npm=1
  fi

  if [ "${need_py}" -eq 1 ] && [ "${need_npm}" -eq 1 ]; then
    log_info "Running first-time / updated project setup..."
    "${ROOT_DIR}/scripts/setup.sh"
  else
    if [ "${need_py}" -eq 1 ]; then
      log_info "Updating Python backend virtual environment..."
      if [ ! -d "${ROOT_DIR}/backend/.venv" ]; then
        python3 -m venv "${ROOT_DIR}/backend/.venv"
      fi
      "${ROOT_DIR}/backend/.venv/bin/pip" install -r "${ROOT_DIR}/backend/requirements.txt"
    fi
    if [ "${need_npm}" -eq 1 ]; then
      log_info "Installing frontend dependencies..."
      if [ -f "${ROOT_DIR}/frontend/package-lock.json" ]; then
        (cd "${ROOT_DIR}/frontend" && npm ci)
      else
        (cd "${ROOT_DIR}/frontend" && npm install)
      fi
    fi
    if [ "${need_py}" -eq 0 ] && [ "${need_npm}" -eq 0 ]; then
      log_ok "Python venv and node_modules up to date (reused)"
    fi
  fi

  printf "%s" "${req_hash}" >"${STATE_DIR}/req.sha256"
  printf "%s" "${pkg_hash}" >"${STATE_DIR}/pkg.sha256"
}

ensure_engine() {
  local engine_bin="${ROOT_DIR}/engine/build/ghostfs_engine"
  local needs_build=0

  if [ ! -x "${engine_bin}" ] || [ ! -f "${ROOT_DIR}/engine/build/CMakeCache.txt" ]; then
    needs_build=1
  elif [ -n "$(find "${ROOT_DIR}/engine/include" "${ROOT_DIR}/engine/src" "${ROOT_DIR}/engine/CMakeLists.txt" -newer "${engine_bin}" -print -quit 2>/dev/null)" ]; then
    needs_build=2
  fi

  if [ "${needs_build}" -eq 1 ]; then
    log_info "Configuring and compiling C++17 engine..."
    cmake -S "${ROOT_DIR}/engine" -B "${ROOT_DIR}/engine/build"
    cmake --build "${ROOT_DIR}/engine/build" -j4
  elif [ "${needs_build}" -eq 2 ]; then
    log_info "C++ source changes detected; rebuilding incrementally..."
    cmake --build "${ROOT_DIR}/engine/build" -j4
  fi

  local ping_out
  ping_out="$(printf '{"command":"ping"}\n' | "${engine_bin}" 2>/dev/null || true)"
  if ! printf "%s" "${ping_out}" | grep -q '"ok"[[:space:]]*:[[:space:]]*true'; then
    log_err "Engine validation failed (ghostfs_engine did not return ok=true)."
    exit 1
  fi
  log_ok "C++17 engine ready (${engine_bin})"
}

is_pid_alive() {
  local pid_file="$1"
  [ -f "${pid_file}" ] || return 1
  local pid
  pid="$(cat "${pid_file}" 2>/dev/null || true)"
  [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null
}

port_occupied_pid() {
  local port="$1"
  local is_bound
  is_bound="$(python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.4); rc=s.connect_ex(('127.0.0.1', int(sys.argv[1]))); s.close(); print('1' if rc==0 else '')" "${port}" 2>/dev/null || true)"
  if [ -z "${is_bound}" ]; then
    return 0
  fi
  local lsof_bin=""
  if command -v lsof >/dev/null 2>&1; then
    lsof_bin="lsof"
  elif [ -x "/usr/sbin/lsof" ]; then
    lsof_bin="/usr/sbin/lsof"
  fi
  if [ -n "${lsof_bin}" ]; then
    local pid
    pid="$("${lsof_bin}" -nP -iTCP:"${port}" -sTCP:LISTEN -t 2>/dev/null | head -n 1 || true)"
    printf "%s" "${pid:-unknown}"
  else
    printf "unknown"
  fi
}

http_get_ok() {
  local url="$1"
  local pattern="${2:-}"
  python3 -c '
import sys, urllib.request
url = sys.argv[1]
pat = sys.argv[2] if len(sys.argv) > 2 else ""
try:
    with urllib.request.urlopen(url, timeout=1.5) as resp:
        if resp.status != 200:
            sys.exit(1)
        body = resp.read().decode("utf-8", errors="ignore")
        if pat and pat not in body:
            sys.exit(1)
        sys.exit(0)
except Exception:
    sys.exit(1)
' "${url}" "${pattern}"
}

open_browser() {
  local url="$1"
  if [ "${NO_BROWSER}" -eq 1 ] || [ "${GHOSTFS_NO_BROWSER:-0}" = "1" ]; then
    return 0
  fi
  if [ "${OS_NAME}" = "macOS" ] && command -v open >/dev/null 2>&1; then
    open "${url}" >/dev/null 2>&1 || true
  elif [ "${OS_NAME}" = "Linux" ] && command -v xdg-open >/dev/null 2>&1; then
    if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
      xdg-open "${url}" >/dev/null 2>&1 || true
    fi
  fi
}

cleanup_on_failure() {
  log_warn "Cleaning up partially started GhostFS processes..."
  "${ROOT_DIR}/scripts/stop.sh" >/dev/null 2>&1 || true
  for logf in "${PID_DIR}/backend.log" "${PID_DIR}/frontend.log"; do
    if [ -f "${logf}" ]; then
      printf "\n--- Tail of %s ---\n" "${logf}" >&2
      tail -n 20 "${logf}" >&2 || true
    fi
  done
}

start_services() {
  local start_ts
  start_ts="$(date +%s)"

  local backend_alive=0 frontend_alive=0
  is_pid_alive "${PID_DIR}/backend.pid" && backend_alive=1
  is_pid_alive "${PID_DIR}/frontend.pid" && frontend_alive=1

  if [ "${backend_alive}" -eq 1 ] && [ "${frontend_alive}" -eq 1 ]; then
    if http_get_ok "http://127.0.0.1:${BACKEND_PORT}/api/v1/health" '"ok"' && http_get_ok "http://127.0.0.1:${FRONTEND_PORT}/"; then
      log_warn "GhostFS is already running."
      log_ok "Backend  : http://127.0.0.1:${BACKEND_PORT} (PID $(cat "${PID_DIR}/backend.pid"))"
      log_ok "Frontend : http://127.0.0.1:${FRONTEND_PORT} (PID $(cat "${PID_DIR}/frontend.pid"))"
      return 0
    fi
  fi

  if [ "${backend_alive}" -eq 0 ]; then
    local occ_b
    occ_b="$(port_occupied_pid "${BACKEND_PORT}")"
    if [ -n "${occ_b}" ]; then
      log_err "Port ${BACKEND_PORT} is occupied by an unrelated process (PID: ${occ_b})."
      log_err "GhostFS will not terminate unrelated processes. Stop it or set GHOSTFS_BACKEND_PORT."
      exit 1
    fi
  fi

  if [ "${frontend_alive}" -eq 0 ]; then
    local occ_f
    occ_f="$(port_occupied_pid "${FRONTEND_PORT}")"
    if [ -n "${occ_f}" ]; then
      log_err "Port ${FRONTEND_PORT} is occupied by an unrelated process (PID: ${occ_f})."
      log_err "GhostFS will not terminate unrelated processes. Stop it or set GHOSTFS_FRONTEND_PORT."
      exit 1
    fi
  fi

  log_info "Starting GhostFS backend (port ${BACKEND_PORT}) and frontend (port ${FRONTEND_PORT})..."
  if ! "${ROOT_DIR}/scripts/start.sh"; then
    cleanup_on_failure
    exit 1
  fi

  local waited=0 max_wait=25 backend_ok=0 frontend_ok=0
  while [ "${waited}" -lt "${max_wait}" ]; do
    if ! is_pid_alive "${PID_DIR}/backend.pid" || ! is_pid_alive "${PID_DIR}/frontend.pid"; then
      log_err "A GhostFS server process exited unexpectedly during startup."
      cleanup_on_failure
      exit 1
    fi
    if [ "${backend_ok}" -eq 0 ] && http_get_ok "http://127.0.0.1:${BACKEND_PORT}/api/v1/health" '"engine_reachable"'; then
      backend_ok=1
    fi
    if [ "${frontend_ok}" -eq 0 ] && http_get_ok "http://127.0.0.1:${FRONTEND_PORT}/"; then
      frontend_ok=1
    fi
    if [ "${backend_ok}" -eq 1 ] && [ "${frontend_ok}" -eq 1 ]; then
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done

  if [ "${backend_ok}" -ne 1 ] || [ "${frontend_ok}" -ne 1 ]; then
    log_err "Health verification timed out after ${max_wait}s (backend_ok=${backend_ok}, frontend_ok=${frontend_ok})."
    cleanup_on_failure
    exit 1
  fi

  local elapsed
  elapsed=$(( $(date +%s) - start_ts ))
  log_ok "Backend  : http://127.0.0.1:${BACKEND_PORT}/api/v1/health (PID $(cat "${PID_DIR}/backend.pid"))"
  log_ok "Frontend : http://127.0.0.1:${FRONTEND_PORT} (PID $(cat "${PID_DIR}/frontend.pid"))"
  printf "\n%sGhostFS Ready%s in %ss -> %shttp://127.0.0.1:%s%s\n" \
    "${C_GREEN}${C_BOLD}" "${C_RESET}" "${elapsed}" "${C_CYAN}${C_BOLD}" "${FRONTEND_PORT}" "${C_RESET}"

  open_browser "http://127.0.0.1:${FRONTEND_PORT}"
}

foreground_wait() {
  local stopping=0
  handle_signal() {
    if [ "${stopping}" -eq 0 ]; then
      stopping=1
      printf "\n"
      log_info "Stopping GhostFS-owned servers..."
      "${ROOT_DIR}/scripts/stop.sh"
      exit 0
    fi
  }
  trap handle_signal INT TERM

  printf "%sPress Ctrl+C to stop GhostFS (or run './start.sh stop' from another terminal).%s\n" "${C_DIM}" "${C_RESET}"
  while true; do
    if ! is_pid_alive "${PID_DIR}/backend.pid" || ! is_pid_alive "${PID_DIR}/frontend.pid"; then
      log_warn "A GhostFS server stopped unexpectedly. Cleaning up..."
      "${ROOT_DIR}/scripts/stop.sh" >/dev/null 2>&1 || true
      exit 1
    fi
    sleep 2
  done
}

cmd_status() {
  detect_platform
  print_banner
  local engine_bin="${ROOT_DIR}/engine/build/ghostfs_engine"
  if [ -x "${engine_bin}" ] && printf '{"command":"ping"}\n' | "${engine_bin}" 2>/dev/null | grep -q '"ok"[[:space:]]*:[[:space:]]*true'; then
    log_ok "Engine   : Built and responsive (${engine_bin})"
  else
    log_warn "Engine   : Not built or unresponsive"
  fi

  if is_pid_alive "${PID_DIR}/backend.pid" && http_get_ok "http://127.0.0.1:${BACKEND_PORT}/api/v1/health" '"ok"'; then
    log_ok "Backend  : Running (PID $(cat "${PID_DIR}/backend.pid"), http://127.0.0.1:${BACKEND_PORT})"
  else
    log_warn "Backend  : Stopped"
  fi

  if is_pid_alive "${PID_DIR}/frontend.pid" && http_get_ok "http://127.0.0.1:${FRONTEND_PORT}/"; then
    log_ok "Frontend : Running (PID $(cat "${PID_DIR}/frontend.pid"), http://127.0.0.1:${FRONTEND_PORT})"
  else
    log_warn "Frontend : Stopped"
  fi
}

cmd_logs() {
  local target="${1:-all}"
  case "${target}" in
    backend)
      [ -f "${PID_DIR}/backend.log" ] && tail -n 60 "${PID_DIR}/backend.log" || log_warn "No backend log found."
      ;;
    frontend)
      [ -f "${PID_DIR}/frontend.log" ] && tail -n 60 "${PID_DIR}/frontend.log" || log_warn "No frontend log found."
      ;;
    all)
      printf "=== Backend Log ===\n"
      [ -f "${PID_DIR}/backend.log" ] && tail -n 30 "${PID_DIR}/backend.log" || printf "(empty)\n"
      printf "\n=== Frontend Log ===\n"
      [ -f "${PID_DIR}/frontend.log" ] && tail -n 30 "${PID_DIR}/frontend.log" || printf "(empty)\n"
      ;;
    *)
      log_err "Usage: $0 logs [backend|frontend|all]"
      exit 1
      ;;
  esac
}

cmd_help() {
  cat <<EOF
GhostFS Unified Project Launcher

Usage:
  ./start.sh [--no-browser] [--detach]   Auto-setup, build, start, and wait (Ctrl+C stops)
  ./start.sh start [--no-browser]        Start GhostFS in background
  ./start.sh stop                        Stop GhostFS-owned processes
  ./start.sh restart [--no-browser]      Restart GhostFS-owned processes
  ./start.sh status                      Display engine, backend, and frontend status
  ./start.sh setup                       Install project-local Python & Node dependencies
  ./start.sh build                       Build C++17 engine and React frontend
  ./start.sh test                        Run C++ CTest, FastAPI Pytest, and Frontend build
  ./start.sh demo                        Run complete investigation demonstration
  ./start.sh sanitize [asan|ubsan]       Run sanitizer verification workflow
  ./start.sh logs [backend|frontend|all] Display application logs
  ./start.sh help                        Show this help message

Environment Variables:
  GHOSTFS_BACKEND_PORT=8000              Override backend port
  GHOSTFS_FRONTEND_PORT=5173             Override frontend port
  GHOSTFS_NO_BROWSER=1                   Disable automatic browser launch
EOF
}

# Parse command and flags
CMD="up"
NO_BROWSER=0
DETACH=0

if [ "$#" -gt 0 ]; then
  case "$1" in
    start|stop|restart|status|setup|build|test|demo|sanitize|logs|help|-h|--help)
      CMD="$1"
      shift
      ;;
  esac
fi

ARGS=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --no-browser) NO_BROWSER=1 ;;
    --detach|-d)  DETACH=1 ;;
    *) ARGS+=("$1") ;;
  esac
  shift
done

case "${CMD}" in
  help|-h|--help)
    cmd_help
    ;;
  up)
    detect_platform
    print_banner
    check_prerequisites
    ensure_setup
    ensure_engine
    start_services
    if [ "${DETACH}" -eq 0 ]; then
      foreground_wait
    else
      printf "%sStop anytime with: ./start.sh stop%s\n" "${C_DIM}" "${C_RESET}"
    fi
    ;;
  start)
    detect_platform
    print_banner
    check_prerequisites
    ensure_setup
    ensure_engine
    start_services
    printf "%sStop anytime with: ./start.sh stop%s\n" "${C_DIM}" "${C_RESET}"
    ;;
  stop)
    "${ROOT_DIR}/scripts/stop.sh"
    ;;
  restart)
    "${ROOT_DIR}/scripts/stop.sh"
    detect_platform
    print_banner
    check_prerequisites
    ensure_setup
    ensure_engine
    start_services
    printf "%sStop anytime with: ./start.sh stop%s\n" "${C_DIM}" "${C_RESET}"
    ;;
  status)
    cmd_status
    ;;
  setup)
    detect_platform
    check_prerequisites
    prepare_runtime_dirs
    "${ROOT_DIR}/scripts/setup.sh"
    file_sha256 "${ROOT_DIR}/backend/requirements.txt" >"${STATE_DIR}/req.sha256"
    if [ -f "${ROOT_DIR}/frontend/package-lock.json" ]; then
      file_sha256 "${ROOT_DIR}/frontend/package-lock.json" >"${STATE_DIR}/pkg.sha256"
    else
      file_sha256 "${ROOT_DIR}/frontend/package.json" >"${STATE_DIR}/pkg.sha256"
    fi
    ;;
  build)
    detect_platform
    check_prerequisites
    prepare_runtime_dirs
    "${ROOT_DIR}/scripts/build.sh"
    ;;
  test)
    detect_platform
    check_prerequisites
    ensure_setup
    ensure_engine
    "${ROOT_DIR}/scripts/test.sh"
    ;;
  demo)
    detect_platform
    check_prerequisites
    ensure_setup
    ensure_engine
    "${ROOT_DIR}/scripts/demo.sh"
    ;;
  sanitize)
    detect_platform
    check_prerequisites
    "${ROOT_DIR}/scripts/sanitize.sh" ${ARGS[@]+"${ARGS[@]}"}
    ;;
  logs)
    cmd_logs ${ARGS[@]+"${ARGS[@]}"}
    ;;
esac
