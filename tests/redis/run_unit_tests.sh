#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="${ROOT}/build/cache_server"
PORT="${CACHE_SERVER_REDIS_UNIT_PORT:-6399}"
HOST="127.0.0.1"

if [[ ! -x "${SERVER}" ]]; then
  echo "missing ${SERVER}; build cache_server first" >&2
  exit 2
fi

"${SERVER}" "${PORT}" 1 64 &
SERVER_PID=$!
cleanup() {
  kill "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  if (exec 3<>"/dev/tcp/${HOST}/${PORT}") 2>/dev/null; then
    exec 3<&-
    exec 3>&-
    break
  fi
  sleep 0.1
done

mapfile -t TEST_FILES < <(find "${ROOT}/tests/redis/unit" -type f -name '*.tcl' | sort)
if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
  echo "no redis unit test files found" >&2
  exit 2
fi

tclsh "${ROOT}/tests/redis/harness/cache_server_unit_runner.tcl" \
  "${HOST}" "${PORT}" "${TEST_FILES[@]}"
