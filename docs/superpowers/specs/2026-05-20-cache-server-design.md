# Cache Server Design

Date: 2026-05-20

## Goal

Build a C++17 cache server with Redis RESP2 compatibility for a small Redis 6.2
command subset. The server uses:

- `thirdparty/libco` for coroutine-based networking, following
  `example/example_echosvr.cpp`.
- `thirdparty/cpp_util/redis` for RESP2 pack/unpack.
- `thirdparty/cpp_util/immutable_container` for immutable map/set based data
  versions.
- `thirdparty/jemalloc` for high-performance allocation when enabled by the
  build.

The first version optimizes for high throughput, simple code, clear C++ object
boundaries, slot-level write parallelism, asynchronous master/slave replication,
and in-memory-only operation.

## Non-Goals

- No persistence. Data, TTL metadata, replication backlog, and snapshot state
  live only in memory.
- No `AUTH`, `FLUSHDB`, admin command set, Redis Cluster, Lua, transactions,
  pub/sub, blocking commands, or persistence commands.
- No full Redis replication protocol compatibility. Replication uses RESP2 but
  custom `CACHE.REPL` subcommands.
- No multi-key commands in the first version.

## Supported Commands

Command semantics reference the Redis 6.2 command set. Only these fixed forms
are supported:

- `SET key value`
- `GET key`
- `HSET key field value`
- `HGET key field`
- `SADD key member`
- `SISMEMBER key member`
- `ZADD key score member`
- `ZSCORE key member`
- `DEL key`
- `EXPIRE key seconds`
- `TTL key`

Unsupported optional arguments return a Redis-style arity or syntax error. The
server accepts command names case-insensitively.

Return behavior:

- `SET` overwrites any previous type and clears any previous TTL. It returns
  `+OK`.
- `GET` returns null bulk for missing or expired keys, bulk string for existing
  string keys, and WRONGTYPE for non-string keys.
- `HSET` creates a hash if needed. It returns `1` for a new field and `0` for
  an updated field.
- `HGET` returns null bulk for missing key or field.
- `SADD` supports one member. It returns `1` for a new member and `0` if the
  member already exists.
- `SISMEMBER` returns `1` when the member exists, otherwise `0`.
- `ZADD` supports one `score member` pair. It returns `1` for a new member and
  `0` for an updated score. The score must parse as a finite `double`.
- `ZSCORE` returns the score as a bulk string or null bulk when missing.
- `DEL` supports one key. It returns `1` when an unexpired key is removed,
  otherwise `0`.
- `EXPIRE` stores a relative deadline. `seconds <= 0` deletes the key
  immediately. It returns `1` when applied to an existing unexpired key,
  otherwise `0`.
- `TTL` returns `-2` for missing or expired keys, `-1` for keys without TTL,
  and the remaining seconds for expiring keys.

All type mismatches return:

```text
-WRONGTYPE Operation against a key holding the wrong kind of value
```

## Process And Thread Model

The network model follows `libco/example/example_echosvr.cpp`:

- The main thread creates the listening socket and runs an accept coroutine.
- Accepted fds are assigned to worker threads round-robin.
- Each worker is an OS thread running `co::ThreadWorker::run_loop()`.
- Each worker owns a pool of connection coroutines.
- A connection coroutine reads bytes, appends to a stream buffer, parses all
  complete RESP commands, executes them, and writes RESP responses.

All workers share one `SlotTable` containing `100003` hash slots. A request is
not forwarded to a slot owner. The current connection worker computes the slot
from the key and uses the slot's locks directly.

## Object Boundaries

Use C++17 classes with private state and narrow interfaces. Internal maps,
mutexes, and buffers are not exposed.

- `net::Server` owns listen/accept/worker lifecycle and passes decoded commands
  to the command layer.
- `protocol::RespCodec` wraps RESP2 parsing/packing, stream buffering,
  pipeline handling, and buffer limits.
- `command::CommandDispatcher` parses RESP arrays into strong command objects,
  validates arity, and maps results back to RESP responses.
- `command::RedisCmd` is the polymorphic command interface. Each command class
  implements `ExecCmd(cache::CacheEngine&, TimePoint)` and returns a typed
  result that the protocol layer packs as RESP.
- `cache::CacheEngine` is the main data API. It owns `SlotTable` and exposes
  operations such as `Execute`, `SnapshotSlot`, `ApplySnapshot`, `ApplyBinlog`,
  and `DeleteExpired`.
- `cache::SlotTable` maps keys to slot ids and delegates to `HashSlot`.
- `cache::HashSlot` encapsulates locks, object map, slot sequence, binlog
  buffer, and snapshot helpers.
- `cache::RedisObject` encapsulates object type, TTL metadata, and immutable
  value payloads.
- `repl::MasterReplicator` owns the master-side single-slave replication state
  machine.
- `repl::SlaveReplicator` owns slave receive, apply queue, apply workers, and
  ACK reporting.
- `expire::ExpireSweeper` owns single-threaded expiration cleanup.

Command implementation files are grouped by Redis data structure to keep
ownership clear and avoid one large command file:

- `string_cmd.*`: `SET`, `GET`
- `hash_cmd.*`: `HSET`, `HGET`
- `set_cmd.*`: `SADD`, `SISMEMBER`
- `zset_cmd.*`: `ZADD`, `ZSCORE`
- `key_cmd.*`: `DEL`, `EXPIRE`, `TTL`

`CommandDispatcher` builds the right `RedisCmd` subclass from RESP arguments.
It does not directly manipulate cache internals and does not contain command
business logic beyond lookup, arity checks, and syntax validation.

## Data Model

The slot id is:

```text
slot = hash(key) % 100003
```

Each `HashSlot` stores:

- `write_mutex`
- `value_mutex`
- `redis_obj_map`
- `binlog_buffer`
- `slot_seq`
- `published_seq`

`redis_obj_map` is:

```cpp
ImtMap<PackedString, RedisObject, std::less<PackedString>, AtomicRefCount>
```

When slot-level logic needs to traverse the whole key/object map, it must use
`ImtMap::ForEach` on an `ObjectMap` snapshot. The cache layer should not depend
on lower-level `ImmutableTree` or `ImmutableBlockTree` internals for this map.

Payload representation:

- String: `PackedString`
- Hash: `ImtMap<PackedString, PackedString, ..., AtomicRefCount>`
- Set: `ImtSet<PackedString, ..., AtomicRefCount>`
- ZSet: `ImtMap<PackedString, double, ..., AtomicRefCount>`

The first version only needs `ZADD` and `ZSCORE`, so zset stores member to score
only. It does not maintain a score-ordered index.

TTL is stored in `RedisObject` as an absolute monotonic microsecond deadline.
`0` means no expiration. `EXPIRE` still accepts seconds, but internal comparison
uses microseconds.

RESP `string_view` values are copied into `PackedString` before the input buffer
is mutated or erased.

## Slot Locking And Commit Order

The lock order is fixed.

Write commands:

1. Acquire `write_mutex`.
2. Read the current object/map version under a short `value_mutex` section.
3. Release `value_mutex`.
4. Build the new immutable object/map version and canonical binlog record.
5. Ensure binlog record allocation succeeds and assign `next_seq = slot_seq + 1`.
6. Append the binlog record and set `slot_seq = next_seq` while still holding
   `write_mutex`.
7. Acquire `value_mutex` briefly and publish both the new `redis_obj_map` and
   `published_seq = next_seq`.
8. Release `write_mutex` and return the client response.

This makes a successful client response mean both the master data version and
replication log entry exist in memory. Snapshot code always captures
`redis_obj_map` and `published_seq` together under `value_mutex`, so it never
observes a data version without its matching sequence boundary.

Read commands:

1. Acquire `value_mutex` briefly.
2. Read the object pointer/version.
3. Release `value_mutex`.
4. If the object is expired, respond as if it is missing.

Read paths do not delete expired keys. They avoid upgrading into write paths.

## Expiration

Expiration cleanup is single-threaded.

Expiration behavior has three layers:

- Read commands lazily treat expired keys as missing.
- Write commands clean or overwrite expired target keys while holding the
  slot's `write_mutex`.
- `expire::ExpireSweeper` runs as one background coroutine/thread. It scans
  slots in small batches, calls the normal slot write path for expired keys,
  and records canonical `DEL` binlog entries.

This keeps expiration deterministic enough for replication while preventing
long lock holds.

## Replication Overview

The first version supports one slave. Replication is asynchronous. The master
does not wait for slave ACK before responding to clients.

Replication uses RESP2 frames with one command family:

```text
CACHE.REPL <subcmd> ...
```

Subcommands:

- `HELLO`: slave reports identity and resume state.
- `SNAP`: master sends a snapshot chunk for one slot.
- `SNAP_COMMIT`: master marks the complete slot snapshot boundary.
- `LOG`: master sends one binlog record.
- `ACK`: slave reports applied per-slot seq progress.
- `ONLINE`: master marks that the slave has caught up and is now receiving
  online incremental logs.

Master replication is single-threaded. The master replication thread/coroutine
handles `HELLO`, `ACK`, snapshot chunks, backlog replay, and online `LOG`
sending for the one slave.

Slave replication uses:

- One sync receiver thread/coroutine to parse master frames and enqueue apply
  tasks.
- An apply worker pool of 2 to 4 threads, default 4.
- One ACK coroutine sending aggregated progress about every 50 ms.

Apply tasks are routed by key hash or slot hash to a fixed worker so the same
key is applied in order.

## Replication Consistency

Each slot has a monotonic `slot_seq`. Every mutating command records a canonical
binlog entry, including user writes, `DEL`, `EXPIRE`, and expiration-generated
internal `DEL`.

Snapshot consistency:

1. For a slot, master first drains existing backlog if possible.
2. Master captures the immutable `redis_obj_map` version and its `published_seq`
   under `value_mutex`, then releases the lock immediately.
3. The immutable snapshot version is serialized in chunks after the lock is
   released.
4. Writes that happen after the captured version append to that slot's
   binlog.
5. Master sends `SNAP_COMMIT slot_id base_seq end_seq`.
6. Slave commits the slot snapshot atomically only after all chunks for that
   slot have been applied to the build context.
7. Logs with `seq > end_seq` wait in a pending queue and are applied after
   snapshot commit.

Slave `LOG` apply is idempotent:

- `seq <= applied_seq` is ignored.
- `seq == applied_seq + 1` is applied.
- `seq > applied_seq + 1` is held until missing records arrive.

For TTL replication, master does not send its monotonic deadline. Snapshot and
binlog records encode remaining TTL in microseconds. Slave applies the record
using `slave_now_us + remaining_ttl_us`. If the key is already expired at the
master, master sends a canonical delete.

## Resume And Binlog Cleanup

TCP handles packet loss. Application-level resume handles slow links, timeouts,
disconnects, and reconnects.

Master maintains per-slot ring backlog with `min_seq..max_seq`.

On reconnect, slave sends `HELLO` with per-slot `last_applied_seq` and, for an
incomplete snapshot, the last completed chunk. Master chooses:

- If `last_applied_seq + 1 >= min_seq`, replay missing `LOG` records.
- If one slot is too far behind, resnapshot only that slot.
- If an in-progress snapshot version is still retained, resume from the next
  missing chunk.

Slave sends `ACK` every about 50 ms with applied seq, not merely received seq.
Master cleans a slot's `binlog_buffer` under that slot's `write_mutex` by
removing records with `seq <= acked_seq[slot]`, while preserving any snapshot
resume metadata that is still referenced.

If the slave stops ACKing and a slot backlog reaches its configured memory or
record limit, master marks only that slot as requiring resnapshot.

## Memory And Protocol Limits

Configured limits:

- Maximum RESP bulk string size.
- Maximum RESP array element count.
- Maximum connection stream buffer size.
- Maximum binlog bytes or records per slot.
- Maximum retained snapshot chunk state.

On protocol overflow or malformed RESP, the server returns a protocol error
when possible and closes the connection.

## Build Shape

The root project should gain a normal C++17 build, preferably CMake because
`libco` already provides CMake targets. The build should:

- Build or link `thirdparty/libco` static library.
- Include header-only `cpp_util/redis` and `cpp_util/immutable_container`.
- Optionally link jemalloc when configured.
- Build unit tests, integration tests, and benchmarks.

The existing root Makefile can remain a thin wrapper around CMake targets.

## Testing Strategy

Unit tests:

- RESP pipeline parsing and packing.
- Command arity, unknown command, syntax errors, and WRONGTYPE behavior.
- Redis-compatible return values for the 11 supported commands.
- TTL microsecond boundary behavior and `TTL` return values.
- `HashSlot` write/read behavior, slot seq monotonicity, binlog append, and ACK
  cleanup.

Concurrency tests:

- Multiple workers reading and writing different slots.
- Multiple workers contending on the same slot.
- Write ordering under `write_mutex`.
- No data races under thread sanitizer where practical.

Replication tests:

- Snapshot followed by snapshot-period writes and binlog catch-up.
- Slave reconnect with backlog replay.
- Backlog gap resnapshots only the lagging slot.
- ACK cleanup removes only applied logs.
- Snapshot chunk resume.
- Slave apply worker ordering for the same key.
- Out-of-order `LOG` holdback and gap fill.
- Snapshot commit is atomic and does not ACK before publish.

Integration tests:

- A small RESP client or `redis-cli --raw` exercises all supported commands.
- Master/slave test verifies eventual equality after writes, disconnects, and
  reconnects.

Benchmarks:

- Single worker pipeline QPS.
- Multi-worker read/write QPS.
- Mixed read/write workload.
- Replication enabled versus disabled write latency.
- Expiration sweeper impact under many expiring keys.

## References

- Redis 6.2 command index:
  `https://redis.io/docs/latest/commands/redis-6-2-commands/`
- RESP helpers: `thirdparty/cpp_util/redis/include/redis/resp.h`
- Immutable containers:
  `thirdparty/cpp_util/immutable_container/include/immutable_container/`
- Libco threading and coroutine example:
  `thirdparty/libco/example/example_echosvr.cpp`
