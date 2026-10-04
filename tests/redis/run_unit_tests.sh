#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ $# -ne 0 && $# -ne 2 ]]; then
  echo "usage: $0 [host port]" >&2
  exit 2
fi

HOST="${1:-${CACHE_SERVER_REDIS_UNIT_HOST:-127.0.0.1}}"
PORT="${2:-${CACHE_SERVER_REDIS_UNIT_PORT:-6399}}"

TEST_FILES=()
while IFS= read -r test_file; do
  TEST_FILES+=("${test_file}")
done < <(find "${ROOT}/tests/redis/unit" -type f -name '*.tcl' | sort)
if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
  echo "no redis unit test files found" >&2
  exit 2
fi

tclsh "${ROOT}/tests/redis/harness/cache_server_unit_runner.tcl" \
  "${HOST}" "${PORT}" "${TEST_FILES[@]}"
