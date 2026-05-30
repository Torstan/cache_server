#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/cache_server"
HOST="127.0.0.1"

MASTER_PORT="${CACHE_SERVER_MASTER_PORT:-7401}"
REPLICA_A_PORT="${CACHE_SERVER_REPLICA_A_PORT:-7402}"
REPLICA_B_PORT="${CACHE_SERVER_REPLICA_B_PORT:-7403}"

REDIS_CLI="${REDIS_CLI:-/usr/bin/redis-cli}"

if [[ ! -x "${SERVER}" ]]; then
  echo "missing ${SERVER}; build cache_server first" >&2
  exit 2
fi
if [[ ! -x "${REDIS_CLI}" ]]; then
  echo "missing ${REDIS_CLI}; install redis-tools" >&2
  exit 2
fi

LOG_DIR="$(mktemp -d -t cache-server-repl-test-XXXXXX)"
MASTER_LOG="${LOG_DIR}/master.log"
REPLICA_A_LOG="${LOG_DIR}/replica_a.log"
REPLICA_B_LOG="${LOG_DIR}/replica_b.log"

MASTER_PID=""
REPLICA_A_PID=""
REPLICA_B_PID=""

cleanup() {
  for pid in "${MASTER_PID}" "${REPLICA_A_PID}" "${REPLICA_B_PID}"; do
    if [[ -n "${pid}" ]]; then
      kill "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${MASTER_PID}" "${REPLICA_A_PID}" "${REPLICA_B_PID}"; do
    if [[ -n "${pid}" ]]; then
      wait "${pid}" 2>/dev/null || true
    fi
  done
  if [[ -n "${KEEP_LOGS:-}" ]]; then
    echo "logs preserved at ${LOG_DIR}" >&2
  else
    rm -rf "${LOG_DIR}"
  fi
}
trap cleanup EXIT

wait_for_port() {
  local port="$1"
  local label="$2"
  for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/${HOST}/${port}") 2>/dev/null; then
      exec 3<&-
      exec 3>&-
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for ${label} on port ${port}" >&2
  return 1
}

expect() {
  local label="$1"
  local expected="$2"
  local actual="$3"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "FAIL [${label}]: expected '${expected}', got '${actual}'" >&2
    exit 1
  fi
}

expect_int_in_range() {
  local label="$1"
  local lo="$2"
  local hi="$3"
  local actual="$4"
  if ! [[ "${actual}" =~ ^-?[0-9]+$ ]]; then
    echo "FAIL [${label}]: expected integer in [${lo},${hi}], got '${actual}'" >&2
    exit 1
  fi
  if (( actual < lo || actual > hi )); then
    echo "FAIL [${label}]: expected integer in [${lo},${hi}], got ${actual}" >&2
    exit 1
  fi
}

expect_contains() {
  local label="$1"
  local needle="$2"
  local actual="$3"
  if [[ "${actual}" != *"${needle}"* ]]; then
    echo "FAIL [${label}]: expected output to contain '${needle}', got '${actual}'" >&2
    exit 1
  fi
}

cli_master() { "${REDIS_CLI}" -h "${HOST}" -p "${MASTER_PORT}" "$@"; }
cli_replica_a() { "${REDIS_CLI}" -h "${HOST}" -p "${REPLICA_A_PORT}" "$@"; }
cli_replica_b() { "${REDIS_CLI}" -h "${HOST}" -p "${REPLICA_B_PORT}" "$@"; }

# Launch master
"${SERVER}" "${MASTER_PORT}" 1 64 >"${MASTER_LOG}" 2>&1 &
MASTER_PID=$!

wait_for_port "${MASTER_PORT}" "master"

# Launch replicas (with --replica-reads to allow GET-style verification)
"${SERVER}" "${REPLICA_A_PORT}" 1 64 \
  --replica \
  "--master=${HOST}:${MASTER_PORT}" \
  --replica-id=replica-a \
  --replica-reads \
  >"${REPLICA_A_LOG}" 2>&1 &
REPLICA_A_PID=$!

"${SERVER}" "${REPLICA_B_PORT}" 1 64 \
  --replica \
  "--master=${HOST}:${MASTER_PORT}" \
  --replica-id=replica-b \
  --replica-reads \
  >"${REPLICA_B_LOG}" 2>&1 &
REPLICA_B_PID=$!

wait_for_port "${REPLICA_A_PORT}" "replica-a"
wait_for_port "${REPLICA_B_PORT}" "replica-b"

# Issue ~25 writes against the master.
expect "master SET k1"   "OK" "$(cli_master SET k1 v1)"
expect "master SET k2"   "OK" "$(cli_master SET k2 v2)"
expect "master DEL k2"   "1"  "$(cli_master DEL k2)"
expect "master SET k3"   "OK" "$(cli_master SET k3 hello)"
expect "master APPEND k3" "11" "$(cli_master APPEND k3 ' world')"
expect "master SET k4"   "OK" "$(cli_master SET k4 100)"
expect "master INCR k4"  "101" "$(cli_master INCR k4)"
expect "master INCRBY k4" "111" "$(cli_master INCRBY k4 10)"

expect "master HSET h1 a"     "1" "$(cli_master HSET h1 a 1)"
expect "master HSET h1 b"     "1" "$(cli_master HSET h1 b 2)"
expect "master HSET h1 c"     "1" "$(cli_master HSET h1 c 3)"
expect "master HDEL h1 b"     "1" "$(cli_master HDEL h1 b)"

expect "master SADD s1 m1"    "1" "$(cli_master SADD s1 m1)"
expect "master SADD s1 m2"    "1" "$(cli_master SADD s1 m2)"
expect "master SADD s1 m3"    "1" "$(cli_master SADD s1 m3)"
expect "master SREM s1 m3"    "1" "$(cli_master SREM s1 m3)"

expect "master ZADD z1 m1"    "1" "$(cli_master ZADD z1 1.5 m1)"
expect "master ZADD z1 m2"    "1" "$(cli_master ZADD z1 2 m2)"
expect "master ZADD z1 m3"    "1" "$(cli_master ZADD z1 3 m3)"
expect "master ZREM z1 m3"    "1" "$(cli_master ZREM z1 m3)"

expect "master SET ttlkey"    "OK" "$(cli_master SET ttlkey v EX 100)"
expect "master EXPIRE k1"     "1"  "$(cli_master EXPIRE k1 200)"

expect "master SET disposable" "OK" "$(cli_master SET disposable bye)"
expect "master DEL disposable" "1"  "$(cli_master DEL disposable)"

# Wait for asynchronous replication (link reconnects every ~1s). Poll
# sentinels from the verified final state so we do not read before trailing
# slot logs, such as SET+DEL, have both applied.
wait_for_convergence() {
  local port="$1"
  local label="$2"
  local cli=("${REDIS_CLI}" -h "${HOST}" -p "${port}")
  local deadline=$(( SECONDS + 30 ))
  local got_k4=""
  local got_disposable=""
  while (( SECONDS < deadline )); do
    got_k4="$("${cli[@]}" GET k4 2>&1 || true)"
    got_disposable="$("${cli[@]}" EXISTS disposable 2>&1 || true)"
    if [[ "${got_k4}" == "111" && "${got_disposable}" == "0" ]]; then
      return 0
    fi
    sleep 0.2
  done
  echo "FAIL [${label}]: timed out waiting for replica convergence (k4='${got_k4}', disposable='${got_disposable}')" >&2
  exit 1
}

wait_for_convergence "${REPLICA_A_PORT}" "replica-a"
wait_for_convergence "${REPLICA_B_PORT}" "replica-b"

verify_replica() {
  local port="$1"
  local label="$2"
  local cli=("${REDIS_CLI}" -h "${HOST}" -p "${port}")

  expect "${label} GET k1"        "v1"          "$("${cli[@]}" GET k1)"
  expect "${label} EXISTS k2"     "0"           "$("${cli[@]}" EXISTS k2)"
  expect "${label} GET k3"        "hello world" "$("${cli[@]}" GET k3)"
  expect "${label} GET k4"        "111"         "$("${cli[@]}" GET k4)"

  expect "${label} HGET h1 a"     "1"           "$("${cli[@]}" HGET h1 a)"
  expect "${label} HGET h1 c"     "3"           "$("${cli[@]}" HGET h1 c)"
  expect "${label} HEXISTS h1 b"  "0"           "$("${cli[@]}" HEXISTS h1 b)"

  expect "${label} SISMEMBER s1 m1" "1" "$("${cli[@]}" SISMEMBER s1 m1)"
  expect "${label} SISMEMBER s1 m2" "1" "$("${cli[@]}" SISMEMBER s1 m2)"
  expect "${label} SISMEMBER s1 m3" "0" "$("${cli[@]}" SISMEMBER s1 m3)"

  expect "${label} ZSCORE z1 m1"  "1.5" "$("${cli[@]}" ZSCORE z1 m1)"
  expect "${label} ZSCORE z1 m2"  "2"   "$("${cli[@]}" ZSCORE z1 m2)"
  local z1_m3_score
  z1_m3_score="$("${cli[@]}" ZSCORE z1 m3)"
  expect "${label} ZSCORE z1 m3 (deleted)" "" "${z1_m3_score}"

  local ttl_v
  ttl_v="$("${cli[@]}" TTL ttlkey)"
  expect_int_in_range "${label} TTL ttlkey" 1 100 "${ttl_v}"

  local ttl_k1
  ttl_k1="$("${cli[@]}" TTL k1)"
  expect_int_in_range "${label} TTL k1" 1 200 "${ttl_k1}"

  expect "${label} EXISTS disposable" "0" "$("${cli[@]}" EXISTS disposable)"
}

verify_replica "${REPLICA_A_PORT}" "replica-a"
verify_replica "${REPLICA_B_PORT}" "replica-b"

# Replicas must reject writes with READONLY.
readonly_out_a="$(cli_replica_a SET should_fail x 2>&1 || true)"
expect_contains "replica-a write rejection" "READONLY" "${readonly_out_a}"
readonly_out_b="$(cli_replica_b SET should_fail x 2>&1 || true)"
expect_contains "replica-b write rejection" "READONLY" "${readonly_out_b}"

echo "replication integration test PASSED"
