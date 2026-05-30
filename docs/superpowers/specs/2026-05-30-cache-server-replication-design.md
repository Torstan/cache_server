# Cache Server Replication Design

## Goal

Add asynchronous single-master, multi-replica data replication to `cache_server`.
The first version must keep the main write path low latency, support reconnect
after short network interruptions, and converge replicas to the master's state
using slot-level snapshot plus slot-level binlog replication.

This design targets the commands currently implemented as single-key writes.
Multi-key or cross-slot atomic replication, failover, persistence, compression,
encryption, and authentication are outside this first version.

## Current State

The codebase already has the data structure foundations:

- Keys map to `100003` slots through `Fnva64(key) % kSlotCount`.
- Each `HashSlot` owns `write_mutex_`, `value_mutex_`, `redis_obj_map_`,
  `binlog_buffer_`, `slot_seq_`, and `published_seq_`.
- Slot writes append a `BinlogRecord`, advance `slot_seq_`, and then publish the
  new immutable object map plus `published_seq_`.
- Replication frames already encode `CACHE.REPL LOG` and `CACHE.REPL ACK` in
  RESP style.
- `SlaveReplicator` can apply `LOG` records through `CommandDispatcher` and
  keeps per-slot `applied_seq`.

The missing pieces are the process-level replication link, slot snapshot
serialization and application, multi-replica session management, global binlog
memory budgeting, and server integration.

## Core Semantics

Replication is asynchronous. A master write succeeds once it is committed in the
master slot and added to that slot's binlog. The master does not wait for any
replica ACK before replying to the client.

Each slot has an independent sequence number. A replica is consistent for a slot
only after it has applied the slot snapshot at `base_seq` and every subsequent
`LOG` through its current `applied_seq`. Replica consistency is therefore
slot-scoped during synchronization and full-cluster after all slots are online.

Short network interruptions are handled by reconnecting with the same
`replica_id` and previous `session_id`, while sending all per-slot applied
positions in the `HELLO` frame. The master resumes with `LOG` frames when the
needed binlog remains available, and sends `SNAPSHOT` only for slots that cannot
be resumed from retained logs.

## Protocol

The replication protocol uses RESP arrays with a `CACHE.REPL` command prefix.
The first version has only four frame types:

```text
CACHE.REPL HELLO <replica_id> <proto_version> <prev_session_id> <count> <slot_id> <applied_seq> ...
CACHE.REPL SNAPSHOT <session_id> <slot_id> <base_seq> <payload>
CACHE.REPL LOG <session_id> <slot_id> <seq> <cmd> <ttl_us> <argc> ...args
CACHE.REPL ACK <session_id> <count> <slot_id> <seq> ...
```

`HELLO` is sent by the replica after connecting or reconnecting. First connect
uses an empty `prev_session_id` and `applied_seq = 0` for all slots. Reconnect
uses the remembered `session_id` and the replica's current per-slot applied
positions.

`SNAPSHOT` contains one complete serialized slot image and the image's
`base_seq`. The master obtains `base_seq` from the slot's published sequence at
snapshot time and must retain logs newer than `base_seq` until the target
replica either receives them or that slot is assigned another snapshot.

`LOG` contains one replayable write command. It carries `session_id` so the
replica can reject late frames from an old connection.

`ACK` reports only fully applied slot positions. A replica never ACKs a partial
or failed snapshot.

There is no separate `RESUME` frame. Resume state is carried by `HELLO`, which
keeps the protocol state machine smaller and makes reconnect handling one
entry point.

## Snapshot Atomicity

Snapshot application must be atomic at slot granularity.

The master serializes a slot's `ObjectMap` snapshot into a single payload. The
first implementation should use a simple internal RESP payload describing each
live key, object type, TTL or deadline metadata, and value fields. This keeps the
outer protocol stable while leaving room for a more compact tree serialization
later.

Before accepting a full resync for a slot, the replica marks that slot
`snapshotting`. This logically removes the slot from the readable set when
replica reads are enabled, but it does not publish an empty object map. The
replica first deserializes the payload into a temporary `ObjectMap`. If parsing
fails, the replica discards the temporary state and keeps the previous map
unpublished for reads until the next successful snapshot for that slot.

On successful parse, the replica takes the slot lock and replaces that slot's
old object map in one commit. The same commit sets `applied_seq = base_seq`,
clears pending logs for that slot, and marks the slot ready to catch up from
`base_seq + 1`.

This is the only lazy clearing path. A full resync logically clears and then
physically replaces one slot at a time, not all replica data at once. Other
slots remain online and retain their data.

## Master Design

Add a `ReplicationManager` owned next to `Server`. It accepts or manages replica
connections, parses `CACHE.REPL` frames, and sends frames to each replica. It
does not sit in the synchronous client write response path.

For each replica, the master tracks:

- `replica_id`
- current `session_id`
- per-slot `acked_seq`
- per-slot send state: `need_snapshot` or `streaming`
- outbound write queue
- recent activity timestamp

After receiving `HELLO`, the master creates or resumes the replica session.
For each reported slot position, it decides whether to stream logs from
`applied_seq + 1` or send a slot snapshot. It streams `LOG` frames whenever the
required binlog sequence is available. It sends `SNAPSHOT` only for initial sync,
detected gaps, or slots whose required logs were evicted by budget pressure.

For each `ACK`, the master updates only the matching current session. ACKs from
old sessions are ignored.

## Binlog Budget

Binlog memory is controlled by a global budget, not by a fixed per-slot record
count.

`BinlogBuffer` records an estimated byte cost for each `BinlogRecord`, and a
global `BinlogBudget` tracks total retained binlog bytes across all slots.

Normal cleanup is safe cleanup: for a slot, the master may remove log records
only up to the minimum ACK among replicas that still need that slot's logs.
Fast replicas must not cause logs required by slow replicas to be removed.

If total retained bytes exceed the global budget and safe cleanup is
insufficient, the master chooses lagging replica slots to resnapshot. For those
replica-slot pairs, the master no longer needs to preserve the old log range.
The affected slot on that replica is marked `need_snapshot`; other slots and
other replicas continue streaming normally.

This policy bounds memory while preserving correctness. Budget pressure may
increase snapshot traffic for lagging replicas, but it does not silently create
inconsistent replicas.

## Replica Design

A replica starts in replica mode, connects to the configured master, and sends
`HELLO`. The first version rejects local write commands in replica mode to avoid
master-replica divergence.

Each replica slot tracks:

- `state`: `offline`, `snapshotting`, `catching_up`, or `online`
- `applied_seq`
- `pending_logs`

On `SNAPSHOT`, the replica validates `session_id`, marks the slot
`snapshotting`, parses into a temporary map, and commits the map atomically on
success. After commit it drains any pending consecutive logs. If no gap remains,
the slot becomes `online`; otherwise it becomes `catching_up`.

On `LOG`, the replica validates `session_id` and slot id. If `seq` is already
applied, it ignores the frame and may ACK the current position. If `seq` is the
next expected sequence, it replays the command through `CommandDispatcher`,
advances `applied_seq`, and drains pending logs. If `seq` is ahead of the next
expected sequence, it stores the log in `pending_logs`. If pending logs exceed a
limit or remain gapped too long, the slot moves to `offline` and waits for a
snapshot.

If command replay fails, the replica does not advance `applied_seq`. It marks
the slot offline and waits for a snapshot.

## Replica Reads

Replica reads are configurable. By default, a replica is a hot standby and does
not serve read traffic.

If replica reads are enabled, a read is allowed only when the target slot is
`online`. Reads for `offline`, `snapshotting`, or `catching_up` slots return a
temporary error such as:

```text
-TRYAGAIN slot is syncing
```

This prevents clients from observing mixed slot state while still allowing lazy
slot resync without clearing the entire replica.

## Network Recovery

Network interruptions of one to two minutes should normally resume without full
resync. The replica reconnects with the same `replica_id`, previous
`session_id`, and its per-slot `applied_seq` values in `HELLO`.

The master resumes each slot independently:

- If `applied_seq + 1` is still retained, the master sends `LOG` frames.
- If the required log range is missing, the master sends `SNAPSHOT` only for
  that slot.

Late frames from the old connection are ignored because `SNAPSHOT`, `LOG`, and
`ACK` all carry `session_id`.

Snapshot payload transfer is not resumable in the first version. If a snapshot
frame is interrupted, the replica does not commit it, reconnects, and receives
that slot snapshot again if needed.

## Server Integration

`Server` should route `CACHE.REPL` frames to replication code instead of the
ordinary command dispatcher. Normal client commands continue through
`CommandDispatcher`.

Command-line or config support should include at least:

- server role: master or replica
- replica master host and port
- replica id
- replica read mode
- global binlog budget

## Testing

Unit and integration tests should cover:

- `LOG` replication for current single-key write command families.
- Snapshot encode, decode, and atomic slot replacement.
- Bad snapshot payload does not replace the old slot image.
- Lazy slot clearing affects only the resynced slot.
- Reconnect with `HELLO` resumes from retained binlog after a short disconnect.
- Binlog gap triggers `SNAPSHOT` only for the affected slot.
- Multi-replica ACK cleanup keeps logs needed by slow replicas.
- Global budget pressure can force a lagging replica slot to resnapshot.
- Old-session `SNAPSHOT`, `LOG`, and `ACK` are ignored.
- Replica-read mode reads only `online` slots and returns `TRYAGAIN` for syncing
  slots.
- End-to-end master plus two replicas eventually converge after writes, one
  replica disconnect, and reconnect.

## Out of Scope

The first implementation does not include:

- Multi-key or cross-slot atomic command replication.
- Snapshot payload chunking or snapshot transfer resume.
- Automatic failover or leader election.
- Disk-persistent sessions or slot progress; process restart can resnapshot.
- Authentication, encryption, or compression for replication traffic.
