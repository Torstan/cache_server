# Redis Core Commands Design

## Goal

Implement the common string, hash, set, and sorted set Redis commands in this
server while matching Redis 6.2 command syntax, return shapes, and error
behavior for every command that is registered.

## Supported Command Scope

This pass supports commands whose full Redis 6.2 semantics can be implemented
against the current object model with focused cache changes:

- String: `SET`, `GET`, `MGET`, `SETNX`, `GETSET`, `STRLEN`, `APPEND`,
  `INCR`, `DECR`, `INCRBY`, `DECRBY`.
- Hash: `HSET`, `HGET`, `HDEL`, `HEXISTS`, `HLEN`, `HSTRLEN`, `HMGET`,
  `HMSET`, `HGETALL`, `HKEYS`, `HVALS`, `HINCRBY`.
- Set: `SADD`, `SISMEMBER`, `SREM`, `SCARD`, `SMEMBERS`, `SMISMEMBER`,
  `SPOP`, `SRANDMEMBER`.
- Sorted set: `ZADD`, `ZSCORE`, `ZREM`, `ZCARD`, `ZRANK`, `ZREVRANK`,
  `ZCOUNT`, `ZINCRBY`, `ZRANGE`.

Existing commands in this set are upgraded where needed, for example multi
field/member forms for `HSET`, `SADD`, and `ZADD`, and Redis 6.2 options for
`SET`, `ZADD`, and `ZRANGE`.

## Architecture

Command parsing and Redis semantics stay in `src/command/*_cmd.*`.
`CommandDispatcher` registers the new commands and classifies writes for
replication replay. `src/cache` receives only small primitives needed by
command semantics, especially a mutation path that can update or delete a key
under the same slot write lock.

Write commands store their original Redis command argv in `BinlogRecord::args`.
New commands may leave `BinlogRecord::op` unset because replication replay uses
the command name in `args[0]`. This preserves enough information for a later
replication pass without expanding the legacy enum for every Redis command.

## Data Semantics

String reads return null bulk for missing keys and `WRONGTYPE` for non-string
keys. Integer string operations parse signed 64-bit integers and report Redis
style value/overflow errors. `SET` supports Redis 6.2 `NX`, `XX`, `GET`,
`EX`, `PX`, `EXAT`, `PXAT`, and `KEEPTTL`; successful whole-key string writes
clear TTL unless `KEEPTTL` is present. Redis 6.2 rejects the `NX` and `GET`
combination; that combination was only allowed in later Redis versions.

Hash, set, and sorted set member updates preserve key TTL. Removing the final
member deletes the key, matching Redis behavior that empty aggregate values do
not remain as existing keys.

Sorted set ordering is computed from the stored member-score map as needed:
rank order is score ascending then member lexicographic ascending, reverse rank
uses the reverse of that order, and lex ranges use member lexicographic order.

## Error Handling

Registered commands reject wrong arity and invalid options before mutating data.
Unsupported options on supported commands return Redis-style syntax errors.
Type mismatches return the existing Redis-compatible `WRONGTYPE` error string.
Multi-value commands validate all inputs before writing when Redis semantics
require all-or-nothing behavior, such as `ZADD` scores and `HINCRBY` integers.

## Unsupported Commands And Required Future Capabilities

Some Redis 6.2 string/hash/set/zset commands remain unsupported in this pass.
They should not be registered until the missing infrastructure exists.

- Cross-key write and store commands, including `MSET`, `MSETNX`, `SMOVE`,
  `SINTERSTORE`, `SUNIONSTORE`, `SDIFFSTORE`, `ZUNION`, `ZINTER`, `ZDIFF`,
  `ZUNIONSTORE`, `ZINTERSTORE`, `ZDIFFSTORE`, and `ZRANGESTORE`, need
  multi-slot atomic mutation or multi-key snapshot reads, deadlock-free slot
  locking, and binlog records that describe one logical transaction across all
  affected slots.
- Cursor and pattern scan commands, including `HSCAN`, `SSCAN`, and `ZSCAN`,
  need cursor state, glob pattern matching, count hints, and stable iteration
  behavior over immutable snapshots.
- Bit and byte range string commands, including `GETRANGE`, `SETRANGE`,
  `GETBIT`, `SETBIT`, `BITCOUNT`, `BITFIELD`, `BITFIELD_RO`, `BITOP`, and
  `BITPOS`, need byte-addressable string mutation helpers, sparse expansion
  limits, and bit-level parsing/overflow rules.
- Floating point and advanced numeric commands not in scope, such as
  `INCRBYFLOAT` and `HINCRBYFLOAT`, need Redis-compatible double formatting,
  non-finite value rejection, and precise propagation through binlog replay.
- Blocking/pop-move set or sorted-set commands, such as `BZPOPMIN`,
  `BZPOPMAX`, `ZPOPMIN`, and `ZPOPMAX`, need event-loop blocking semantics or
  a deliberate non-blocking subset decision.
- Full set algebra read commands, including `SINTER`, `SUNION`, `SDIFF`, and
  `SINTERCARD`, need efficient multi-key reads and well-defined ordering for
  responses where Redis does not guarantee order.
- Less common sorted set helpers, including `ZLEXCOUNT`, `ZRANGEBYLEX`,
  `ZREVRANGEBYLEX`, `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE`, `ZMSCORE`,
  `ZRANDMEMBER`, `ZREVRANGE`, `ZSCAN`, and `ZREMRANGEBY*`, should be added
  after the shared range parser and mutation-delete primitives are proven by
  `ZRANGE`, `ZCOUNT`, and `ZREM`.

| Command(s) | Reason not supported in this pass | Future capability needed |
| --- | --- | --- |
| `MSET`, `MSETNX`, `SMOVE`, `SINTER`, `SUNION`, `SDIFF`, `SINTERCARD`, `SINTERSTORE`, `SUNIONSTORE`, `SDIFFSTORE`, `ZUNION`, `ZINTER`, `ZDIFF`, `ZUNIONSTORE`, `ZINTERSTORE`, `ZDIFFSTORE`, `ZRANGESTORE` | These commands read or write multiple keys, often across slots. The current command path is centered on one key in one slot at a time. | Multi-key snapshot reads, multi-slot atomic mutation, deterministic deadlock-free slot locking, and transaction-style binlog entries that replay one logical command across all affected slots. |
| `HSCAN`, `SSCAN`, `ZSCAN` | Cursor iteration is more than a container walk: Redis clients expect cursor state, pattern filtering, and count-hint behavior. | Cursor-state encoding, glob pattern matching, count hint handling, and stable iteration over immutable snapshots. |
| `GETRANGE`, `SETRANGE`, `GETBIT`, `SETBIT`, `BITCOUNT`, `BITFIELD`, `BITFIELD_RO`, `BITOP`, `BITPOS` | These operate on byte offsets and bit offsets inside string values. Current string commands treat strings as whole `PackedString` values. | Byte-addressable string mutation helpers, sparse string expansion limits, bit-level parsing, and Redis-compatible overflow rules. |
| `INCRBYFLOAT`, `HINCRBYFLOAT` | Floating point writes need Redis-compatible formatting and must avoid non-finite state in storage and binlog replay. | Shared double parser/formatter, non-finite result rejection, and replay-safe canonical score/value formatting. |
| `SETEX`, `PSETEX`, `GETDEL`, `GETEX`, `HSETNX`, `HRANDFIELD`, `ZMSCORE`, `ZRANDMEMBER`, `ZLEXCOUNT`, `ZRANGE` legacy aliases such as `ZREVRANGE`, `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE`, `ZRANGEBYLEX`, `ZREVRANGEBYLEX`, `ZREMRANGEBYRANK`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYLEX` | These are feasible on a single node, but they would expand this pass beyond the agreed core command set and duplicate parsers/helpers before the first implementation is proven. | Follow-up single-node command pass after shared range parsers, get-and-expire helpers, random-field helpers, legacy alias dispatch, and range-delete mutation helpers are isolated. |
| `ZPOPMIN`, `ZPOPMAX`, `BZPOPMIN`, `BZPOPMAX` | Pop commands need sorted-set min/max removal helpers; blocking variants also need event-loop waiting semantics. | Sorted-set pop primitives plus either true blocking client support or an explicit non-blocking compatibility decision. |

## Testing

Tests cover the public command dispatcher because that is the compatibility
surface used by both clients and replication replay. Each command family gets
focused tests for normal behavior, missing keys, wrong types, invalid syntax,
and TTL preservation or clearing where applicable.
