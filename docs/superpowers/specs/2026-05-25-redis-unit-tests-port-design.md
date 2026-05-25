# Redis Unit Tests Port Design

## Goal

Port Redis 6.2 `tests/unit` coverage for commands supported by this server, without changing existing command behavior except for the newly agreed query commands needed by the test suite. The port should preserve official Redis test block bodies for runnable supported command cases, so failures point to compatibility gaps instead of local rewrites.

The Redis source baseline is `redis/redis` branch or tag `6.2`, under `tests/unit`. `SCAN` semantics follow the Redis command contract described at `https://redis.io/docs/latest/commands/scan/`, with the project-specific cursor simplification described below.

## Scope

The initial supported command scope is the command registry in `src/command/command_dispatcher.cpp`, plus these new read-only commands:

- `EXISTS`
- `TYPE`
- `PTTL`
- `SCAN`

The existing supported commands remain in scope: strings, hashes, sets, sorted sets, `DEL`, `EXPIRE`, and `TTL` as already registered by `CommandDispatcher`.

The following commands and behavior are not in this pass: `FLUSHDB`, `FLUSHALL`, `CONFIG`, `DEBUG`, `OBJECT`, `DBSIZE`, `KEYS`, `RANDOMKEY`, `TIME`, persistence, replication-stream assertions, transactions, blocking clients, ACL, scripting, modules, cluster, pubsub, and Redis internal encoding checks.

## Test Import Rules

Copy Redis 6.2 `tests/unit` into `tests/redis/unit` and filter it into a runnable suite. The copied runnable files keep the upstream directory layout, especially `tests/redis/unit/type/{string,hash,set,zset}.tcl`, plus selected `expire.tcl`, `keyspace.tcl`, and other files when their test blocks target in-scope commands.

A test block is retained when its behavioral subject is an in-scope command and every Redis command it executes is in scope. Retained test block bodies are not edited. If a block targets Redis internal encoding, cleanup, configuration, persistence, replication, transaction, slow/stress behavior, or an unsupported command, delete that block from the runnable copy and record the reason in the manifest.

`tests/redis/unit/manifest.json` records:

- Redis source ref.
- Included files.
- Retained test titles.
- Deleted test titles.
- The unsupported command or non-goal capability that caused each deletion.

This manifest is the audit trail for the requirement that supported command tests remain unmodified. A block that mainly tests a non-goal capability is not considered a supported command behavior test even if it calls an in-scope command as setup.

## Test Harness

Add a lightweight TCL harness under `tests/redis/harness`:

- `resp_client.tcl` opens a socket to `cache_server`, sends RESP2 arrays, and decodes RESP2 replies.
- `cache_server_unit_runner.tcl` provides the subset of Redis test helpers needed by retained blocks: `start_server`, `test`, `assert_equal`, `assert_match`, `assert_error`, `assert_range`, random string helpers, and list comparison helpers.
- `run_unit_tests.sh` builds or locates `build/cache_server`, starts it on an isolated local port, runs selected TCL files, stops the process, and exits nonzero on the first failing test.

The harness must not simulate Redis server commands. Calls through `r <cmd> ...` are sent to `cache_server`. Test framework helpers may compare values and manage the local test process, but they must not fake `FLUSH*`, `CONFIG`, `DEBUG`, `OBJECT`, or query command results.

Add a CTest target named `redis_unit_tests` that runs the shell wrapper. Existing `cache_tests` stay unchanged.

## New Commands

### EXISTS

`EXISTS key [key ...]` returns an integer count of keys that exist and are not expired. Duplicate input keys are counted repeatedly, matching Redis behavior.

### TYPE

`TYPE key` returns a simple string:

- `none` for missing or expired keys.
- `string` for string objects.
- `hash` for hash objects.
- `set` for set objects.
- `zset` for sorted set objects.

List, stream, and other Redis types are not returned because this server does not store them.

### PTTL

`PTTL key` returns the remaining TTL in milliseconds:

- `-2` for missing or expired keys.
- `-1` for keys without an expiration.
- A nonnegative millisecond count for volatile keys.

The implementation should derive this from the existing microsecond object deadline, not from `TTL * 1000`, so millisecond tests can observe sub-second expiration accurately.

### SCAN

`SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]` returns a two-element array: next cursor as a bulk string, and an array of keys.

Cursor encoding is intentionally simple: the cursor is a slot id only. It never embeds a key or slot-local key position, avoiding binary-key encoding problems and cursor payload copying.

Default `COUNT` is `1000`. `COUNT` is parsed and validated as a nonnegative integer. The implementation treats it as a scan work threshold, not a hard return limit:

- Each call scans at least one complete slot.
- A call may scan multiple slots.
- After each full slot, if the number of scanned keys has reached or exceeded `COUNT`, stop and return the next slot id as the cursor.
- If one slot has more than `COUNT` keys, return all matching keys from that slot and stop afterward.
- If the scan reaches the end of the slot range, return cursor `0`.
- With no writes during iteration, repeated `SCAN` calls starting at cursor `0` and stopping when the returned cursor is `0` must return every non-expired key in the DB at least once.

`MATCH` filters returned keys using Redis-style glob matching. `TYPE` filters returned keys by object type. Filtering affects the returned key list but not cursor advancement. Empty key arrays with a nonzero cursor are allowed.

Supported `TYPE` filters are `string`, `hash`, `set`, and `zset`. `none` is accepted but matches no stored key. Other type strings are accepted and also match no stored key, mirroring the Redis `TYPE` filter model where the argument is compared to the type name returned by `TYPE`.

## Common Matching

Implement the glob matcher in `src/common`, not in `src/cache`.

The common interface should be reusable by keyspace scan and future `HSCAN`, `SSCAN`, and `ZSCAN` member scans. It should support byte-oriented `std::string_view` input and Redis glob features used by tests:

- `*`
- `?`
- Character classes with `[...]`
- Negated classes where Redis supports them
- Backslash escapes

The interface may be template based to avoid `std::function` overhead in hot paths. A simple shape is:

```cpp
bool GlobMatch(std::string_view pattern, std::string_view value);

template <typename Emit>
void EmitIfGlobMatches(std::string_view pattern,
                       std::string_view value,
                       Emit&& emit);
```

Command and cache code can layer `std::function` adapters on top if a runtime callback is more convenient, but the core matcher should avoid allocation and key-list materialization.

## Cache Interfaces

Add read-only cache traversal APIs so command code does not copy a full DB key list before filtering.

`HashSlot` should expose a const traversal method that visits non-expired objects in that slot and passes the key and object type to a callback. `CacheEngine` should expose a scan helper that iterates slots from a cursor and invokes a caller-provided emit callback for matching keys.

The scan path should:

- Read each slot through an immutable snapshot or existing lock-safe traversal pattern.
- Skip expired keys.
- Apply optional type filtering before glob matching when possible.
- Apply `common::GlobMatch` without copying the key unless the key is emitted.
- Return the next slot cursor and scan progress metadata to `ScanCmd`.

This design keeps command parsing in `src/command`, data traversal in `src/cache`, and pattern matching in `src/common`.

## Error Handling

New commands follow existing command error style:

- Wrong arity: `ERR wrong number of arguments for '<lowercase>' command`.
- Invalid integer cursor or count: `ERR value is not an integer or out of range`.
- Invalid `SCAN` option combination or unknown option: `ERR syntax error`.
- `COUNT` values that do not parse as nonnegative integers are rejected.
- `COUNT 0` is accepted as a hint, but the project invariant still applies: every `SCAN` call scans at least one complete slot.
- `SCAN` cursor outside the slot range is rejected as an invalid cursor.

Read-only commands do not create binlog records and are registered as non-write commands.

## Verification

Add focused C++ tests for the new commands before relying on TCL coverage:

- `EXISTS` counts existing, missing, expired, and duplicate keys.
- `TYPE` returns all supported stored types and `none`.
- `PTTL` returns `-2`, `-1`, and millisecond TTL ranges.
- `SCAN` handles cursor parsing, default count, explicit count, cross-slot scanning, empty slots, `MATCH`, `TYPE`, and full stable iteration.
- Glob matcher tests cover wildcard, character class, escaping, binary-safe values, and non-matches.

Run:

- `cmake --build build --target cache_tests cache_server`
- `ctest --test-dir build --output-on-failure -R cache_tests`
- `ctest --test-dir build --output-on-failure -R redis_unit_tests`

The Redis TCL suite is considered passing only when all retained official test blocks pass without body modifications.

## Non-Goals

This work does not add Redis cleanup commands, persistence commands, management commands, internal encoding visibility, transactions, blocking behavior, or multi-client semantics. It also does not make SCAN cursor behavior byte-for-byte identical to Redis internals; it preserves the Redis iteration contract under a slot-id cursor design chosen for this project.
