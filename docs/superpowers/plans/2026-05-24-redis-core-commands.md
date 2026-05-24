# Redis Core Commands Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Redis 6.2 compatible core command set for string, hash, set, and sorted set values.

**Architecture:** Keep Redis command semantics in `src/command`, add one cache mutation primitive for update-or-delete behavior, and register only commands whose Redis 6.2 syntax is implemented. Every write command stores the original Redis argv in `BinlogRecord::args`; new write commands may leave `BinlogRecord::op` unset because replay already uses `args[0]`.

**Tech Stack:** C++17, existing `CacheEngine`, `HashSlot`, immutable map/set containers, custom `CACHE_TEST` harness, CMake.

---

## Execution Contract

Use the Redis 6.2 command index as the compatibility source:
`https://redis.io/docs/latest/commands/redis-6-2-commands/`.

For individual command pages under `https://redis.io/docs/latest/commands/<cmd>/`,
apply only syntax and options available in Redis 6.2. Ignore options whose
`since` version is newer than Redis 6.2.

Supported in this plan:

- String: `SET`, `GET`, `MGET`, `SETNX`, `GETSET`, `STRLEN`, `APPEND`,
  `INCR`, `DECR`, `INCRBY`, `DECRBY`.
- Hash: `HSET`, `HGET`, `HDEL`, `HEXISTS`, `HLEN`, `HSTRLEN`, `HMGET`,
  `HMSET`, `HGETALL`, `HKEYS`, `HVALS`, `HINCRBY`.
- Set: `SADD`, `SISMEMBER`, `SREM`, `SCARD`, `SMEMBERS`, `SMISMEMBER`,
  `SPOP`, `SRANDMEMBER`.
- Sorted set: `ZADD`, `ZSCORE`, `ZREM`, `ZCARD`, `ZRANK`, `ZREVRANK`,
  `ZCOUNT`, `ZINCRBY`, `ZRANGE`.

Do not register any command outside that list in this implementation pass.
Unsupported commands must remain `ERR unknown command '<COMMAND>'`.

## File Map

- `tests/cache_command_test.cpp`: dispatcher-level compatibility tests for
  return types, wrong types, invalid syntax, TTL effects, binlog args, and
  unsupported command behavior.
- `src/cache/redis_object.h`: define `MutationResult`.
- `src/cache/hash_slot.h` and `src/cache/hash_slot.cpp`: add a slot-local
  `Mutate` operation that can no-op, set, or delete a key while holding the
  write lock.
- `src/cache/cache_engine.h` and `src/cache/cache_engine.cpp`: expose
  `CacheEngine::Mutate`.
- `src/command/string_cmd.h` and `src/command/string_cmd.cpp`: string command
  classes, string option parsing, integer string arithmetic, TTL construction
  for `SET`.
- `src/command/hash_cmd.h` and `src/command/hash_cmd.cpp`: hash command
  classes and hash mutation helpers.
- `src/command/set_cmd.h` and `src/command/set_cmd.cpp`: set command classes
  and deterministic-enough random selection helpers.
- `src/command/zset_cmd.h` and `src/command/zset_cmd.cpp`: sorted set command
  classes, score formatting, range parsing, bound parsing, and sorted views.
- `src/command/command_dispatcher.cpp`: static command instances, registry
  entries, and write classification.

## Shared Compatibility Rules

- Wrong arity: return `ERR wrong number of arguments for '<lowercase>' command`.
- Syntax or invalid option combination: return `ERR syntax error`.
- Wrong type: return the existing string
  `WRONGTYPE Operation against a key holding the wrong kind of value`.
- Integer parse failures for string/hash integer commands: return
  `ERR value is not an integer or out of range`.
- Integer overflow during arithmetic: return
  `ERR increment or decrement would overflow`.
- Invalid float score: return `ERR value is not a valid float`.
- Successful member-level hash/set/zset updates preserve existing key TTL.
- Whole-key string writes clear TTL unless the Redis command explicitly keeps it.
- Removing the final hash field, set member, or sorted-set member deletes the key.
- Multi-value commands validate every argument before writing when Redis requires
  all-or-nothing behavior.

## Task 1: Add Test Helpers And Compatibility Tests

**Files:**
- Modify: `tests/cache_command_test.cpp`

- [ ] **Step 1: Add response helper functions near existing `RequireType`**

Add these helpers exactly once in the anonymous namespace:

```cpp
void RequireInteger(const protocol::Response& response, std::int64_t expected,
                    std::string_view message) {
  RequireType(response, protocol::ResponseType::kInteger, message);
  test::Require(response.integer == expected, message);
}

void RequireBulk(const protocol::Response& response, std::string_view expected,
                 std::string_view message) {
  RequireType(response, protocol::ResponseType::kBulkString, message);
  test::RequireEqual(response.text, expected, message);
}

void RequireNullBulk(const protocol::Response& response,
                     std::string_view message) {
  RequireType(response, protocol::ResponseType::kNullBulkString, message);
}

void RequireSimpleString(const protocol::Response& response,
                         std::string_view expected,
                         std::string_view message) {
  RequireType(response, protocol::ResponseType::kSimpleString, message);
  test::RequireEqual(response.text, expected, message);
}

bool ArrayHasBulkText(const protocol::Response& response,
                      std::string_view expected) {
  if (response.type != protocol::ResponseType::kArray) {
    return false;
  }
  for (const protocol::Response& element : response.elements) {
    if (element.type == protocol::ResponseType::kBulkString &&
        element.text == expected) {
      return true;
    }
  }
  return false;
}
```

- [ ] **Step 2: Add string command tests**

Add a `CACHE_TEST(StringCommandsMatchRedis62CoreSemantics)` that verifies:

```cpp
cache::CacheEngine engine;
command::CommandDispatcher dispatcher;
const std::uint64_t now_us = 1'000'000;

RequireSimpleString(Exec(dispatcher, engine, now_us,
                         {"SET", "s", "one", "EX", "10"}),
                    "OK", "SET accepts EX");
RequireBulk(Exec(dispatcher, engine, now_us, {"SET", "s", "two", "GET"}),
            "one", "SET GET returns previous string");
RequireInteger(Exec(dispatcher, engine, now_us, {"TTL", "s"}), -1,
               "SET without KEEPTTL clears TTL");
RequireSimpleString(Exec(dispatcher, engine, now_us,
                         {"SET", "s", "ttl", "PX", "10000"}),
                    "OK", "SET accepts PX");
RequireSimpleString(Exec(dispatcher, engine, now_us,
                         {"SET", "s", "kept", "XX", "KEEPTTL"}),
                    "OK", "SET XX KEEPTTL updates existing key");
test::Require(Exec(dispatcher, engine, now_us, {"TTL", "s"}).integer > 0,
              "SET KEEPTTL preserves TTL");
RequireNullBulk(Exec(dispatcher, engine, now_us,
                     {"SET", "s", "blocked", "NX", "GET"}),
                "SET NX GET returns null bulk when condition fails");
RequireBulk(Exec(dispatcher, engine, now_us, {"GET", "s"}), "kept",
            "SET NX does not overwrite existing key");
RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "s", "no"}), 0,
               "SETNX existing key returns 0");
RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "new", "yes"}), 1,
               "SETNX missing key returns 1");

auto mget = Exec(dispatcher, engine, now_us, {"MGET", "s", "missing", "new"});
RequireType(mget, protocol::ResponseType::kArray, "MGET returns array");
test::Require(mget.elements.size() == 3, "MGET returns one item per key");
RequireBulk(mget.elements[0], "kept", "MGET returns first value");
RequireNullBulk(mget.elements[1], "MGET missing key returns null");
RequireBulk(mget.elements[2], "yes", "MGET returns last value");

RequireBulk(Exec(dispatcher, engine, now_us, {"GETSET", "new", "old"}),
            "yes", "GETSET returns old value");
RequireInteger(Exec(dispatcher, engine, now_us, {"APPEND", "new", "-tail"}),
               8, "APPEND returns new length");
RequireInteger(Exec(dispatcher, engine, now_us, {"STRLEN", "new"}), 8,
               "STRLEN returns byte length");
RequireInteger(Exec(dispatcher, engine, now_us, {"INCR", "counter"}), 1,
               "INCR creates integer string");
RequireInteger(Exec(dispatcher, engine, now_us, {"INCRBY", "counter", "9"}),
               10, "INCRBY adds delta");
RequireInteger(Exec(dispatcher, engine, now_us, {"DECR", "counter"}), 9,
               "DECR subtracts one");
RequireInteger(Exec(dispatcher, engine, now_us, {"DECRBY", "counter", "4"}),
               5, "DECRBY subtracts delta");
RequireType(Exec(dispatcher, engine, now_us, {"INCRBY", "counter", "bad"}),
            protocol::ResponseType::kError,
            "INCRBY rejects non-integer delta");
```

- [ ] **Step 3: Add hash command tests**

Add a `CACHE_TEST(HashCommandsMatchRedis62CoreSemantics)` that verifies
multi-pair `HSET`, `HMSET`, `HMGET` with null elements, `HDEL`, `HEXISTS`,
`HLEN`, `HSTRLEN`, `HGETALL`, `HKEYS`, `HVALS`, `HINCRBY`, invalid integer
errors, and deletion of the final field. Include these assertions:

```cpp
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"HSET", "h", "a", "1", "b", "two"}), 2,
               "HSET creates two fields");
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"HSET", "h", "a", "3", "c", "4"}), 1,
               "HSET counts only new fields");
RequireSimpleString(Exec(dispatcher, engine, now_us,
                         {"HMSET", "h", "d", "5", "e", "6"}), "OK",
                    "HMSET returns OK");
auto hmget = Exec(dispatcher, engine, now_us,
                  {"HMGET", "h", "a", "missing", "d"});
RequireType(hmget, protocol::ResponseType::kArray, "HMGET returns array");
RequireBulk(hmget.elements[0], "3", "HMGET returns existing field");
RequireNullBulk(hmget.elements[1], "HMGET missing field returns null");
RequireBulk(hmget.elements[2], "5", "HMGET returns final field");
RequireInteger(Exec(dispatcher, engine, now_us, {"HEXISTS", "h", "b"}), 1,
               "HEXISTS finds field");
RequireInteger(Exec(dispatcher, engine, now_us, {"HLEN", "h"}), 5,
               "HLEN counts fields");
RequireInteger(Exec(dispatcher, engine, now_us, {"HSTRLEN", "h", "b"}), 3,
               "HSTRLEN returns byte length");
RequireInteger(Exec(dispatcher, engine, now_us, {"HINCRBY", "h", "a", "7"}),
               10, "HINCRBY increments field");
auto keys = Exec(dispatcher, engine, now_us, {"HKEYS", "h"});
test::Require(ArrayHasBulkText(keys, "a") && ArrayHasBulkText(keys, "e"),
              "HKEYS returns fields");
auto values = Exec(dispatcher, engine, now_us, {"HVALS", "h"});
test::Require(ArrayHasBulkText(values, "10") && ArrayHasBulkText(values, "6"),
              "HVALS returns values");
auto all = Exec(dispatcher, engine, now_us, {"HGETALL", "h"});
test::Require(ArrayHasBulkText(all, "a") && ArrayHasBulkText(all, "10"),
              "HGETALL returns field-value data");
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"HDEL", "h", "a", "missing", "b"}), 2,
               "HDEL removes existing fields");
RequireInteger(Exec(dispatcher, engine, now_us, {"HSET", "single", "x", "y"}),
               1, "HSET creates single-field hash");
RequireInteger(Exec(dispatcher, engine, now_us, {"HDEL", "single", "x"}), 1,
               "HDEL removes final field");
RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "single", "str"}),
               1, "empty hash key is deleted");
```

- [ ] **Step 4: Add set command tests**

Add a `CACHE_TEST(SetCommandsMatchRedis62CoreSemantics)` that verifies:

```cpp
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"SADD", "s", "a", "b", "c", "a"}), 3,
               "SADD accepts multiple members");
RequireInteger(Exec(dispatcher, engine, now_us, {"SCARD", "s"}), 3,
               "SCARD returns cardinality");
auto flags = Exec(dispatcher, engine, now_us, {"SMISMEMBER", "s", "a", "x"});
RequireType(flags, protocol::ResponseType::kArray, "SMISMEMBER returns array");
RequireInteger(flags.elements[0], 1, "SMISMEMBER marks existing member");
RequireInteger(flags.elements[1], 0, "SMISMEMBER marks missing member");
auto members = Exec(dispatcher, engine, now_us, {"SMEMBERS", "s"});
test::Require(ArrayHasBulkText(members, "a") && ArrayHasBulkText(members, "c"),
              "SMEMBERS returns members");
RequireInteger(Exec(dispatcher, engine, now_us, {"SREM", "s", "b", "x"}), 1,
               "SREM removes existing members");
RequireType(Exec(dispatcher, engine, now_us, {"SRANDMEMBER", "s"}),
            protocol::ResponseType::kBulkString,
            "SRANDMEMBER without count returns bulk string");
auto random_many = Exec(dispatcher, engine, now_us, {"SRANDMEMBER", "s", "3"});
RequireType(random_many, protocol::ResponseType::kArray,
            "SRANDMEMBER with positive count returns array");
test::Require(random_many.elements.size() <= 2,
              "positive SRANDMEMBER count caps at cardinality");
auto popped = Exec(dispatcher, engine, now_us, {"SPOP", "s", "2"});
RequireType(popped, protocol::ResponseType::kArray, "SPOP count returns array");
test::Require(popped.elements.size() == 2, "SPOP pops requested count");
RequireInteger(Exec(dispatcher, engine, now_us, {"SCARD", "s"}), 0,
               "SPOP removes popped members");
RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "s", "str"}), 1,
               "empty set key is deleted");
```

- [ ] **Step 5: Add sorted set command tests**

Add a `CACHE_TEST(ZSetCommandsMatchRedis62CoreSemantics)` that verifies:

```cpp
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"ZADD", "z", "1", "one", "2", "two", "3", "three"}),
               3, "ZADD accepts multiple pairs");
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"ZADD", "z", "NX", "10", "one", "4", "four"}), 1,
               "ZADD NX only adds new members");
RequireBulk(Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "one"}), "1",
            "ZADD NX does not update existing member");
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"ZADD", "z", "XX", "CH", "5", "two", "6", "missing"}),
               1, "ZADD XX CH counts changed existing members");
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"ZADD", "z", "GT", "CH", "4", "one"}), 1,
               "ZADD GT updates greater score");
RequireInteger(Exec(dispatcher, engine, now_us,
                    {"ZADD", "z", "LT", "CH", "3", "one"}), 1,
               "ZADD LT updates lower score");
RequireBulk(Exec(dispatcher, engine, now_us,
                 {"ZADD", "z", "INCR", "2", "one"}), "5",
            "ZADD INCR returns new score");
RequireBulk(Exec(dispatcher, engine, now_us, {"ZINCRBY", "z", "1.5", "one"}),
            "6.5", "ZINCRBY returns new score");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZCARD", "z"}), 4,
               "ZCARD returns cardinality");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZRANK", "z", "three"}), 0,
               "ZRANK returns ascending rank");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZREVRANK", "z", "one"}), 0,
               "ZREVRANK returns descending rank");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZCOUNT", "z", "(3", "+inf"}),
               3, "ZCOUNT supports exclusive and infinity bounds");
auto range = Exec(dispatcher, engine, now_us,
                  {"ZRANGE", "z", "0", "-1", "WITHSCORES"});
RequireType(range, protocol::ResponseType::kArray,
            "ZRANGE WITHSCORES returns array");
test::Require(range.elements.size() == 8, "ZRANGE returns member-score pairs");
RequireBulk(range.elements[0], "three", "ZRANGE first member");
RequireBulk(range.elements[1], "3", "ZRANGE first score");
auto by_score = Exec(dispatcher, engine, now_us,
                     {"ZRANGE", "z", "+inf", "(4", "BYSCORE", "REV",
                      "LIMIT", "0", "2"});
RequireType(by_score, protocol::ResponseType::kArray,
            "ZRANGE BYSCORE REV LIMIT returns array");
test::Require(by_score.elements.size() == 2, "ZRANGE LIMIT caps result");
RequireBulk(by_score.elements[0], "one", "ZRANGE BYSCORE REV first member");
auto by_lex = Exec(dispatcher, engine, now_us,
                   {"ZRANGE", "z", "[o", "+", "BYLEX"});
test::Require(ArrayHasBulkText(by_lex, "one") &&
              ArrayHasBulkText(by_lex, "three") &&
              ArrayHasBulkText(by_lex, "two"),
              "ZRANGE BYLEX returns lex range members");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZREM", "z", "one", "none"}),
               1, "ZREM removes existing members");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZADD", "single-z", "1", "m"}),
               1, "ZADD creates single-member zset");
RequireInteger(Exec(dispatcher, engine, now_us, {"ZREM", "single-z", "m"}), 1,
               "ZREM removes final member");
RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "single-z", "str"}),
               1, "empty zset key is deleted");
```

- [ ] **Step 6: Add binlog and write classification tests**

Extend `CommandDispatcherClassifiesWriteCommands` to assert that these are
writes:

```cpp
for (std::string_view cmd :
     {"SETNX", "GETSET", "APPEND", "INCR", "DECR", "INCRBY", "DECRBY",
      "HDEL", "HMSET", "HINCRBY", "SREM", "SPOP", "ZREM", "ZINCRBY"}) {
  test::Require(dispatcher.IsWriteCommand(cmd), "new write command classified");
}
```

Add a `CACHE_TEST(NewWriteCommandsStoreReplayableArgv)` that executes
`HDEL`, `SREM`, `ZREM`, `APPEND`, and `ZINCRBY`, then checks each affected
slot has a `BinlogRecord` whose `args[0]` is the command name and whose args
match the original normalized argv used by the command.

- [ ] **Step 7: Add unsupported command tests**

Add `CACHE_TEST(UnsupportedRedis62CommandsRemainUnregistered)` with:

```cpp
for (std::string_view cmd :
     {"MSET", "MSETNX", "GETDEL", "GETEX", "GETRANGE", "SETRANGE",
      "GETBIT", "SETBIT", "BITCOUNT", "BITFIELD", "BITFIELD_RO", "BITOP",
      "BITPOS", "INCRBYFLOAT", "HSETNX", "HINCRBYFLOAT", "HRANDFIELD",
      "HSCAN", "SINTER", "SUNION", "SDIFF", "SINTERCARD", "SMOVE",
      "SSCAN", "ZMSCORE", "ZPOPMIN", "ZPOPMAX", "BZPOPMIN", "BZPOPMAX",
      "ZRANGEBYSCORE", "ZREVRANGEBYSCORE", "ZRANGEBYLEX",
      "ZREVRANGEBYLEX", "ZREMRANGEBYRANK", "ZREMRANGEBYSCORE",
      "ZREMRANGEBYLEX", "ZLEXCOUNT", "ZRANGESTORE", "ZUNIONSTORE",
      "ZINTERSTORE", "ZDIFFSTORE", "ZSCAN"}) {
  auto response = Exec(dispatcher, engine, now_us, {cmd, "k"});
  RequireType(response, protocol::ResponseType::kError,
              "unsupported command returns error");
  test::Require(response.text.find("ERR unknown command") == 0,
                "unsupported command remains unregistered");
}
```

- [ ] **Step 8: Verify tests fail before implementation**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected before implementation: build succeeds, `cache_tests` fails on the new
compatibility tests because commands are unknown or existing commands reject
Redis 6.2 arity/options.

## Task 2: Add Cache Mutation Primitive

**Files:**
- Modify: `src/cache/redis_object.h`
- Modify: `src/cache/hash_slot.h`
- Modify: `src/cache/hash_slot.cpp`
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`
- Test: `tests/cache_engine_generic_test.cpp`

- [ ] **Step 1: Add cache mutation tests**

In `tests/cache_engine_generic_test.cpp`, add tests that call
`CacheEngine::Mutate` directly:

```cpp
CACHE_TEST(GenericMutateDeletesExistingKey) {
  cache::CacheEngine engine;
  const std::uint64_t now = 10;
  engine.Set("k", cache::RedisObject::MakeString("v"),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}), now);
  auto result = engine.Mutate(
      "k",
      [](std::optional<cache::RedisObject>) {
        return cache::MutationResult{true, std::nullopt};
      },
      MakeRecord(cache::BinlogOp::kDel, {"CUSTOMDEL", "k"}), now);
  test::Require(result.changed, "Mutate delete changes key");
  test::Require(!engine.Get("k", now).has_value(), "Mutate deletes key");
}

CACHE_TEST(GenericMutateNoOpDoesNotAppendLog) {
  cache::CacheEngine engine;
  const std::uint64_t now = 10;
  auto result = engine.Mutate(
      "missing",
      [](std::optional<cache::RedisObject>) {
        return cache::MutationResult{false, std::nullopt};
      },
      MakeRecord(cache::BinlogOp::kSet, {"NOOP", "missing"}), now);
  test::Require(!result.changed, "Mutate no-op reports unchanged");
  test::Require(engine.SlotForKey("missing").CopyLogsAfter(0, 10).empty(),
                "Mutate no-op does not append binlog");
}
```

- [ ] **Step 2: Run mutation tests to verify RED**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected: build fails because `cache::MutationResult` and
`CacheEngine::Mutate` are not defined.

- [ ] **Step 3: Define `MutationResult`**

In `src/cache/redis_object.h`, after `class RedisObject`, add:

```cpp
struct MutationResult {
  bool changed = false;
  std::optional<RedisObject> object;
};
```

- [ ] **Step 4: Add public mutation API**

In `src/cache/hash_slot.h`, add:

```cpp
WriteResult Mutate(
    std::string_view key,
    std::function<MutationResult(std::optional<RedisObject>)> mutator,
    BinlogRecord record, std::uint64_t now_us);
```

In `src/cache/cache_engine.h`, add the same signature on `CacheEngine`.

- [ ] **Step 5: Implement `HashSlot::Mutate` semantics**

Implement these exact semantics in `src/cache/hash_slot.cpp`:

- Load the existing object only if it exists and is not expired at `now_us`.
- Call `mutator(existing)`.
- If `changed == false`, return `WriteResult{Status::kOk, false, false, slot_seq_}` and do not append a binlog record.
- If `changed == true` and `object.has_value()`, set the key to that object,
  append `record`, publish the new map, and set `created` when no non-expired
  object existed before mutation.
- If `changed == true` and `object == std::nullopt`, erase the key, append
  `record`, and publish the new map. If there was no non-expired object, return
  unchanged and do not append a binlog record.

Update `HashSlot::Update` to delegate to `Mutate` while preserving existing
`Update` behavior: an updater returning `std::nullopt` remains a no-op, not a
delete.

- [ ] **Step 6: Implement `CacheEngine::Mutate` pass-through**

In `src/cache/cache_engine.cpp`, add:

```cpp
WriteResult CacheEngine::Mutate(
    std::string_view key,
    std::function<MutationResult(std::optional<RedisObject>)> mutator,
    BinlogRecord record, std::uint64_t now_us) {
  return SlotForKey(key).Mutate(key, std::move(mutator), std::move(record),
                                now_us);
}
```

- [ ] **Step 7: Verify GREEN for cache primitive**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected after this task: cache primitive tests pass. Command compatibility
tests still fail until command tasks are implemented.

## Task 3: Implement String Commands

**Files:**
- Modify: `src/command/string_cmd.h`
- Modify: `src/command/string_cmd.cpp`
- Modify: `src/command/command_dispatcher.cpp`
- Test: `tests/cache_command_test.cpp`

- [ ] **Step 1: Add command classes**

Declare classes in `src/command/string_cmd.h` for:

```cpp
class MGetCmd;
class SetNxCmd;
class GetSetCmd;
class StrLenCmd;
class AppendCmd;
class IncrCmd;
class DecrCmd;
class IncrByCmd;
class DecrByCmd;
```

Each class follows the existing `GetCmd` style, overriding `CheckArity` and
`ExecCmd`, except write commands override `ExecWithResult` when they mutate.

- [ ] **Step 2: Implement shared string helpers**

In `src/command/string_cmd.cpp`, add helpers with these responsibilities:

- `MakeRecord(command, args)` returns `BinlogRecord` with `args[0]` uppercased
  and the remaining arguments copied exactly as executed.
- `ReadStringObject(engine, key, now_us)` returns missing, wrong type, or the
  stored string.
- `ParseIntegerString(text, out)` uses `common::ParseInt64`.
- `CheckedAddInt64(left, right, out)` rejects signed 64-bit overflow.
- `ApplyStringDeadline(object, deadline_us, keep_ttl, existing_deadline)` sets
  the correct TTL for `SET`.

- [ ] **Step 3: Upgrade `SET`**

Implement Redis 6.2 syntax:

```text
SET key value [NX | XX] [GET] [EX seconds | PX milliseconds | EXAT unix-time-seconds | PXAT unix-time-milliseconds | KEEPTTL]
```

Rules:

- `NX` and `XX` are mutually exclusive.
- Redis 6.2 rejects `NX` with `GET`; keep that combination as `ERR syntax error`.
- Only one expiration option is allowed.
- Expiration numeric values must be valid integers and positive.
- `EX` and `PX` are relative to `now_us`.
- `EXAT` and `PXAT` are absolute Unix deadlines.
- `KEEPTTL` preserves the old deadline only when a key existed and was not expired.
- Without `GET`, successful writes return `+OK`; failed `NX` or `XX` conditions return null bulk.
- With `GET`, return the previous string as bulk, or null bulk if the key did
  not exist or the condition failed. If the previous value exists and is not a
  string, return `WRONGTYPE` and do not mutate.
- Successful `SET` overwrites any type when `GET` is not requested.

- [ ] **Step 4: Implement remaining string commands**

Implement:

- `MGET key [key ...]`: array of bulk/null elements; wrong-type elements are null, matching Redis `MGET`.
- `SETNX key value`: integer 1 if key was set, 0 if key existed.
- `GETSET key value`: bulk old string or null; wrong type returns `WRONGTYPE`; successful write clears TTL.
- `STRLEN key`: integer 0 for missing key; wrong type returns `WRONGTYPE`.
- `APPEND key value`: creates missing string, appends to existing string, returns new length, preserves TTL on existing key.
- `INCR`, `DECR`, `INCRBY`, `DECRBY`: create missing key as `0` before applying delta, return integer new value, preserve TTL on existing key, reject invalid integer strings and overflow.

- [ ] **Step 5: Register string commands**

In `CommandDispatcher::CommandDispatcher`, add static instances and registry
entries. Mark writes: `SET`, `SETNX`, `GETSET`, `APPEND`, `INCR`, `DECR`,
`INCRBY`, `DECRBY`. Mark reads: `GET`, `MGET`, `STRLEN`.

- [ ] **Step 6: Verify string tests**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected after this task: string compatibility assertions pass. Hash, set, and
sorted-set compatibility assertions still fail if those tasks are not done.

## Task 4: Implement Hash Commands

**Files:**
- Modify: `src/command/hash_cmd.h`
- Modify: `src/command/hash_cmd.cpp`
- Modify: `src/command/command_dispatcher.cpp`
- Test: `tests/cache_command_test.cpp`

- [ ] **Step 1: Add command classes**

Declare classes for `HDelCmd`, `HExistsCmd`, `HLenCmd`, `HStrLenCmd`,
`HMGetCmd`, `HMSetCmd`, `HGetAllCmd`, `HKeysCmd`, `HValsCmd`, and
`HIncrByCmd`.

- [ ] **Step 2: Upgrade `HSET`**

`HSET key field value [field value ...]` requires at least one field/value pair
and an even number of field/value arguments. It returns the number of fields
newly added. It preserves existing TTL.

- [ ] **Step 3: Implement hash reads**

Implement:

- `HEXISTS key field`: integer 1 or 0; missing key returns 0.
- `HLEN key`: field count; missing key returns 0.
- `HSTRLEN key field`: byte length; missing key or field returns 0.
- `HMGET key field [field ...]`: array of bulk/null elements.
- `HGETALL key`: array alternating field and value. Use the immutable map's
  in-order traversal; tests must not depend on Redis hash iteration order.
- `HKEYS key`: array of fields.
- `HVALS key`: array of values.

- [ ] **Step 4: Implement hash writes**

Implement:

- `HMSET key field value [field value ...]`: same mutation as `HSET`, returns `+OK`.
- `HDEL key field [field ...]`: returns removed field count; deletes key when final field is removed.
- `HINCRBY key field increment`: parses existing field as signed 64-bit integer or uses 0 for missing field, rejects invalid values and overflow, stores decimal result, returns integer result.

- [ ] **Step 5: Register hash commands**

Mark writes: `HSET`, `HMSET`, `HDEL`, `HINCRBY`. Mark reads: `HGET`,
`HEXISTS`, `HLEN`, `HSTRLEN`, `HMGET`, `HGETALL`, `HKEYS`, `HVALS`.

- [ ] **Step 6: Verify hash tests**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected after this task: string and hash compatibility assertions pass. Set
and sorted-set assertions still fail if those tasks are not done.

## Task 5: Implement Set Commands

**Files:**
- Modify: `src/command/set_cmd.h`
- Modify: `src/command/set_cmd.cpp`
- Modify: `src/command/command_dispatcher.cpp`
- Test: `tests/cache_command_test.cpp`

- [ ] **Step 1: Add command classes**

Declare classes for `SRemCmd`, `SCardCmd`, `SMembersCmd`, `SMIsMemberCmd`,
`SPopCmd`, and `SRandMemberCmd`.

- [ ] **Step 2: Upgrade `SADD`**

`SADD key member [member ...]` requires at least one member and returns the
number of newly added members. It preserves TTL.

- [ ] **Step 3: Implement set reads**

Implement:

- `SCARD key`: cardinality, 0 for missing key.
- `SMEMBERS key`: array of all members. Use in-order traversal; Redis does not
  guarantee set ordering, so tests should check membership rather than exact order.
- `SMISMEMBER key member [member ...]`: array of integer 1/0 flags.
- `SISMEMBER key member`: keep existing behavior.
- `SRANDMEMBER key [count]`: without count returns one bulk member or null bulk for missing/empty set. With positive count returns up to count unique members. With negative count returns exactly `abs(count)` elements and may repeat. With count 0 returns an empty array.

- [ ] **Step 4: Implement set writes**

Implement:

- `SREM key member [member ...]`: returns removed count; deletes key when final member is removed.
- `SPOP key [count]`: without count returns one popped bulk member or null bulk. With count returns an array. Count must be a non-negative integer; count 0 returns an empty array. Delete the key when final member is popped.

Use `std::mt19937_64` with `std::random_device` for selection. Tests must only
assert shape, membership, and cardinality effects.

- [ ] **Step 5: Register set commands**

Mark writes: `SADD`, `SREM`, `SPOP`. Mark reads: `SISMEMBER`, `SCARD`,
`SMEMBERS`, `SMISMEMBER`, `SRANDMEMBER`.

- [ ] **Step 6: Verify set tests**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected after this task: string, hash, and set compatibility assertions pass.
Sorted-set assertions still fail if that task is not done.

## Task 6: Implement Sorted Set Commands

**Files:**
- Modify: `src/command/zset_cmd.h`
- Modify: `src/command/zset_cmd.cpp`
- Modify: `src/command/command_dispatcher.cpp`
- Test: `tests/cache_command_test.cpp`

- [ ] **Step 1: Add command classes**

Declare classes for `ZRemCmd`, `ZCardCmd`, `ZRankCmd`, `ZRevRankCmd`,
`ZCountCmd`, `ZIncrByCmd`, and `ZRangeCmd`.

- [ ] **Step 2: Extend score parsing and formatting**

`ZADD`, `ZINCRBY`, `ZCOUNT`, and `ZRANGE BYSCORE` need score parsing that
accepts finite numbers plus `+inf` and `-inf` where Redis permits infinities.
Stored scores for `ZADD` and `ZINCRBY` must reject non-finite results. Keep
`FormatScore` as the single formatter for score strings.

- [ ] **Step 3: Build sorted views**

In `zset_cmd.cpp`, add local helpers:

- `EntriesByScore(zset)`: vector of `{member, score}`, sorted by score
  ascending then member binary lexicographic ascending.
- `EntriesByScoreReverse(zset)`: reverse of `EntriesByScore`.
- `EntriesByLex(zset)`: vector sorted by member binary lexicographic ascending.
- `AppendRangeResponse(entries, with_scores)`: returns a RESP array of members,
  and interleaves formatted score bulk strings when `WITHSCORES` is set.

- [ ] **Step 4: Upgrade `ZADD`**

Implement Redis 6.2 syntax:

```text
ZADD key [NX | XX] [GT | LT] [CH] [INCR] score member [score member ...]
```

Rules:

- `NX`, `GT`, and `LT` are mutually exclusive. `XX` can combine with `GT` or `LT`.
- `INCR` permits exactly one score/member pair.
- Without `CH`, integer reply counts newly added members.
- With `CH`, integer reply counts new members plus existing members whose score changed.
- `NX` does not update existing members.
- `XX` does not add new members.
- `GT` updates existing members only when the new score is greater; it still adds missing members unless `XX` is also set.
- `LT` updates existing members only when the new score is less; it still adds missing members unless `XX` is also set.
- `INCR` behaves like `ZINCRBY` and returns bulk string new score, or null bulk if options prevent the update.

- [ ] **Step 5: Implement sorted-set writes**

Implement:

- `ZREM key member [member ...]`: removed count; delete key when final member is removed.
- `ZINCRBY key increment member`: add increment to existing score or 0 for missing member, return bulk string new score, preserve TTL.

- [ ] **Step 6: Implement sorted-set reads**

Implement:

- `ZCARD key`: cardinality, 0 for missing key.
- `ZRANK key member`: integer rank or null bulk if missing.
- `ZREVRANK key member`: integer reverse rank or null bulk if missing.
- `ZCOUNT key min max`: support inclusive numeric bounds, exclusive bounds with
  leading `(`, `-inf`, and `+inf`.
- `ZRANGE key start stop [BYSCORE | BYLEX] [REV] [LIMIT offset count] [WITHSCORES]`.

`ZRANGE` details:

- Rank mode parses `start` and `stop` as signed indexes, supports negative
  indexes, and treats `stop` as inclusive.
- `BYSCORE` parses score bounds with inclusive/exclusive syntax and infinities.
- `BYLEX` parses lex bounds: `-`, `+`, `[value`, and `(value`.
- `REV` reverses the selected order before applying `LIMIT`.
- `LIMIT` is valid only with `BYSCORE` or `BYLEX`; parse offset and count as integers.
- `WITHSCORES` interleaves member and score bulk strings in rank and score modes.
  For `BYLEX`, return scores too when Redis 6.2 accepts `WITHSCORES`; if manual
  verification against Redis 6.2 shows `BYLEX WITHSCORES` is rejected, return
  `ERR syntax error` and add a test for that rejection.

- [ ] **Step 7: Register sorted-set commands**

Mark writes: `ZADD`, `ZREM`, `ZINCRBY`. Mark reads: `ZSCORE`, `ZCARD`,
`ZRANK`, `ZREVRANK`, `ZCOUNT`, `ZRANGE`.

- [ ] **Step 8: Verify sorted-set tests**

Run:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected after this task: all command compatibility tests pass except failures
introduced by unsupported-command documentation updates.

## Task 7: Update Unsupported Command Documentation

**Files:**
- Modify: `docs/superpowers/specs/2026-05-24-redis-core-commands-design.md`
- Test: `tests/cache_command_test.cpp`

- [ ] **Step 1: Replace unsupported categories with a concrete table**

In the spec's unsupported section, keep the category explanation and add a
table with these columns:

```markdown
| Command(s) | Reason not supported in this pass | Future capability needed |
| --- | --- | --- |
```

Rows must include:

- `MSET`, `MSETNX`, `SMOVE`, `SINTER`, `SUNION`, `SDIFF`, `SINTERCARD`,
  `SINTERSTORE`, `SUNIONSTORE`, `SDIFFSTORE`, `ZUNION`, `ZINTER`, `ZDIFF`,
  `ZUNIONSTORE`, `ZINTERSTORE`, `ZDIFFSTORE`, `ZRANGESTORE`: multi-key or
  store commands need multi-key snapshot reads, multi-slot atomic mutation,
  and transaction-style binlog.
- `HSCAN`, `SSCAN`, `ZSCAN`: cursor iteration needs cursor state, glob matching,
  count hints, and stable snapshot semantics.
- `GETRANGE`, `SETRANGE`, `GETBIT`, `SETBIT`, `BITCOUNT`, `BITFIELD`,
  `BITFIELD_RO`, `BITOP`, `BITPOS`: byte and bit operations need
  byte-addressable string mutation and bit-level overflow rules.
- `INCRBYFLOAT`, `HINCRBYFLOAT`: float writes need Redis-compatible double
  formatting, non-finite rejection, and replay-safe formatting.
- `SETEX`, `PSETEX`, `GETDEL`, `GETEX`, `HSETNX`, `HRANDFIELD`, `ZMSCORE`,
  `ZRANDMEMBER`, `ZLEXCOUNT`, `ZREVRANGE`, `ZRANGEBYSCORE`,
  `ZREVRANGEBYSCORE`, `ZRANGEBYLEX`, `ZREVRANGEBYLEX`,
  `ZREMRANGEBYRANK`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYLEX`: feasible
  single-node commands omitted to keep this pass bounded; add them after
  shared range, random-field, legacy alias, and get-and-expire helpers are
  isolated.
- `ZPOPMIN`, `ZPOPMAX`, `BZPOPMIN`, `BZPOPMAX`: pop and blocking commands need
  sorted-set pop helpers and event-loop blocking semantics or an explicit
  non-blocking subset decision.

- [ ] **Step 2: Keep unsupported tests aligned with the table**

Ensure `UnsupportedRedis62CommandsRemainUnregistered` includes one command from
every table row and every omitted command named in the table.

- [ ] **Step 3: Verify documentation has no vague unsupported entries**

Run:

```bash
rg "T[B]D|T[O]DO|implement l[a]ter|unsupported with[o]ut|n[o]t sure|m[a]ybe" docs/superpowers/specs/2026-05-24-redis-core-commands-design.md docs/superpowers/plans/2026-05-24-redis-core-commands.md
```

Expected: no matches.

## Task 8: Final Verification

**Files:**
- Review: `docs/superpowers/specs/2026-05-24-redis-core-commands-design.md`
- Review: `docs/superpowers/plans/2026-05-24-redis-core-commands.md`
- Review: `tests/cache_command_test.cpp`
- Review: `src/command/command_dispatcher.cpp`

- [ ] **Step 1: Run full build**

```bash
cmake --build build -j
```

Expected: exit code 0.

- [ ] **Step 2: Run full test suite**

```bash
cd build && ctest --output-on-failure
```

Expected: exit code 0 and all tests pass.

- [ ] **Step 3: Inspect command registration**

Run:

```bash
rg "commands_\\[|static .*Cmd" src/command/command_dispatcher.cpp
```

Expected: only supported commands from this plan are registered, plus pre-existing key commands `DEL`, `EXPIRE`, and `TTL`.

- [ ] **Step 4: Inspect source changes**

Run:

```bash
git diff -- src/cache src/command tests/cache_command_test.cpp docs/superpowers/specs/2026-05-24-redis-core-commands-design.md
```

Expected: changes are limited to the files named in this plan and directly
trace to the supported command set, cache mutation primitive, tests, and
unsupported-command documentation.

- [ ] **Step 5: Final report**

Report:

- implemented command list grouped by string/hash/set/zset;
- unsupported command groups and future capabilities;
- build command and result;
- test command and result;
- any Redis 6.2 semantic deviations discovered during implementation.
