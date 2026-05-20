#!/usr/bin/env bash
set -euo pipefail

port="${1:-6380}"
requests="${2:-10000}"

if ! command -v redis-benchmark >/dev/null 2>&1; then
  echo "redis-benchmark not installed; skipping benchmark"
  exit 0
fi

redis-benchmark -h 127.0.0.1 -p "${port}" -n "${requests}" -t set,get -P 64 -q
