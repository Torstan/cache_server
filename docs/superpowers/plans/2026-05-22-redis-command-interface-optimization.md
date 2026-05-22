# Redis Command Interface Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor Redis command execution to use stateless, reusable command objects with O(1) dispatch and generic CacheEngine interface

**Architecture:** Three-layer design with command layer (stateless commands, validation), storage layer (generic RedisObject operations plus caller-supplied binlog metadata), and utility layer (shared parsing functions). Commands registered in map at startup, eliminating per-request allocations. Storage writes remain replication-safe because every generic write receives the exact `BinlogRecord` that slaves need to replay.

**Tech Stack:** C++17, CMake, CTest, existing immutable container library

**Design corrections relative to the spec:**
- `CacheEngine::Get` returns `std::optional<RedisObject>` (copy) instead of `RedisObject*`, because `HashSlot` uses immutable containers — any concurrent write creates a new map, invalidating internal pointers. The existing code already copies objects out under lock (see `HashSlot::GetString`).
- Generic `Set` and `Update` accept a caller-supplied `BinlogRecord`. A generic storage layer cannot infer whether a write should replay as `SET`, `HSET`, `SADD`, or `ZADD`, nor can it reconstruct command arguments such as value, field, score, or member.
- `CommandDispatcher` keeps a compatibility overload for `std::vector<std::string>` because `RespCodec::CommandArgs` currently owns command arguments as `std::vector<std::string>`.

---

## File Structure

**New Files:**
- `src/common/parse_utils.h` - Parsing utilities
- `src/common/parse_utils.cpp` - Implementation
- `tests/parse_utils_test.cpp` - Tests
- `tests/cache_engine_generic_test.cpp` - Direct tests for generic CacheEngine writes and caller-supplied binlog records

**Modified Files:**
- `src/cache/cache_engine.h` - Replace command-specific methods with generic Get/Set/Update
- `src/cache/cache_engine.cpp` - Implement generic methods (delegate to HashSlot)
- `src/cache/hash_slot.h` - Add generic Get/Set/Update methods
- `src/cache/hash_slot.cpp` - Implement generic methods
- `src/repl/slave_replicator.cpp` - Replay binlog records through generic CacheEngine API before old methods are removed
- `src/command/redis_cmd.h` - New interface: CheckArity + ExecCmd(args)
- `src/command/command_dispatcher.h` - Add command registry
- `src/command/command_dispatcher.cpp` - Registry-based dispatch
- `src/command/string_cmd.h` - Stateless
- `src/command/string_cmd.cpp` - Use CacheEngine generic API
- `src/command/hash_cmd.h` - Stateless
- `src/command/hash_cmd.cpp` - Use CacheEngine generic API
- `src/command/set_cmd.h` - Stateless
- `src/command/set_cmd.cpp` - Use CacheEngine generic API
- `src/command/zset_cmd.h` - Stateless
- `src/command/zset_cmd.cpp` - Use CacheEngine generic API
- `src/command/key_cmd.h` - Stateless
- `src/command/key_cmd.cpp` - Use CacheEngine generic API
- `tests/cache_command_test.cpp` - Update for new interface
- `tests/expire_test.cpp` - Replace removed CacheEngine string helpers with generic Set/Get
- `tests/repl_test.cpp` - Cover generic binlog replay for SET/HSET/SADD/ZADD/EXPIRE/DEL

---

## Task 1: Create Parse Utilities

**Files:** Create `src/common/parse_utils.h`, `src/common/parse_utils.cpp`, `tests/parse_utils_test.cpp`

- [ ] **Step 1: Create src/common/parse_utils.h**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace common {

bool ParseInt64(std::string_view text, std::int64_t* out);
bool ParseFiniteDouble(std::string_view text, double* out);
std::string ToUpperAscii(std::string_view text);

}  // namespace common
```

- [ ] **Step 2: Create src/common/parse_utils.cpp**

```cpp
#include "common/parse_utils.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace common {

bool ParseInt64(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  std::string str(text);
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(str.c_str(), &end, 10);
  if (errno == ERANGE || end == str.c_str() || *end != '\0') return false;
  *out = static_cast<std::int64_t>(value);
  return true;
}

bool ParseFiniteDouble(std::string_view text, double* out) {
  if (text.empty()) return false;
  std::string str(text);
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(str.c_str(), &end);
  if (errno == ERANGE || end == str.c_str() || *end != '\0' || !std::isfinite(value))
    return false;
  *out = value;
  return true;
}

std::string ToUpperAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    result.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A')
                                            : static_cast<char>(ch));
  }
  return result;
}

}  // namespace common
```

- [ ] **Step 3: Create tests/parse_utils_test.cpp**

```cpp
#include "test_harness.h"
#include "common/parse_utils.h"
#include <cmath>
#include <cstdint>
#include <string>

CACHE_TEST(ParseInt64Valid) {
  std::int64_t v = 0;
  test::Require(common::ParseInt64("123", &v), "positive");
  test::Require(v == 123, "value 123");
  test::Require(common::ParseInt64("-456", &v), "negative");
  test::Require(v == -456, "value -456");
  test::Require(common::ParseInt64("0", &v), "zero");
  test::Require(v == 0, "value 0");
}

CACHE_TEST(ParseInt64Invalid) {
  std::int64_t v = 0;
  test::Require(!common::ParseInt64("", &v), "empty");
  test::Require(!common::ParseInt64("abc", &v), "alpha");
  test::Require(!common::ParseInt64("12.34", &v), "decimal");
  test::Require(!common::ParseInt64("123abc", &v), "trailing");
}

CACHE_TEST(ParseFiniteDoubleValid) {
  double v = 0.0;
  test::Require(common::ParseFiniteDouble("3.14", &v), "decimal");
  test::Require(std::abs(v - 3.14) < 0.001, "value 3.14");
  test::Require(common::ParseFiniteDouble("-2.5", &v), "negative");
  test::Require(std::abs(v + 2.5) < 0.001, "value -2.5");
}

CACHE_TEST(ParseFiniteDoubleInvalid) {
  double v = 0.0;
  test::Require(!common::ParseFiniteDouble("", &v), "empty");
  test::Require(!common::ParseFiniteDouble("abc", &v), "alpha");
  test::Require(!common::ParseFiniteDouble("inf", &v), "inf");
  test::Require(!common::ParseFiniteDouble("nan", &v), "nan");
}

CACHE_TEST(ToUpperAscii) {
  test::RequireEqual(common::ToUpperAscii("hello"), std::string("HELLO"), "lower");
  test::RequireEqual(common::ToUpperAscii("WORLD"), std::string("WORLD"), "upper");
  test::RequireEqual(common::ToUpperAscii("set123"), std::string("SET123"), "mixed");
}
```

- [ ] **Step 4: Run tests**

Run: `make test`

- [ ] **Step 5: Commit**

```bash
git add src/common/parse_utils.h src/common/parse_utils.cpp tests/parse_utils_test.cpp
git commit -m "feat: add common parse utilities"
```

---

## Task 2: Add HashSlot and CacheEngine Generic Interfaces

**Files:** Modify `src/cache/hash_slot.h`, `src/cache/hash_slot.cpp`, `src/cache/cache_engine.h`, `src/cache/cache_engine.cpp`. Create `tests/cache_engine_generic_test.cpp`.

**Key design note:** Get returns `std::optional<RedisObject>` (copy), not a pointer. This matches the existing HashSlot pattern where objects are copied out under lock to avoid dangling references to the immutable container's internals.

- [ ] **Step 1: Add declarations to hash_slot.h**

```cpp
// Add after existing public methods in HashSlot class:

  std::optional<RedisObject> Get(std::string_view key, std::uint64_t now_us) const;
  WriteResult Set(std::string_view key, RedisObject obj, BinlogRecord record,
                  std::uint64_t now_us);
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
      BinlogRecord record,
      std::uint64_t now_us);
```

Add `#include <functional>` to hash_slot.h includes.

- [ ] **Step 2: Implement HashSlot::Get**

```cpp
std::optional<RedisObject> HashSlot::Get(std::string_view key,
                                         std::uint64_t now_us) const {
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> value_lock(value_mutex_);
  const RedisObject* found = redis_obj_map_.Find(packed_key);
  if (found == nullptr || found->IsExpired(now_us)) {
    return std::nullopt;
  }
  return *found;
}
```

- [ ] **Step 3: Implement HashSlot::Set**

Follow existing SetString pattern: write_lock -> build next_map -> binlog -> publish.

```cpp
WriteResult HashSlot::Set(std::string_view key, RedisObject obj,
                          BinlogRecord record, std::uint64_t now_us) {
  (void)now_us;
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  ObjectMap next_map;
  bool created = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    created = redis_obj_map_.Find(packed_key) == nullptr;
    next_map = redis_obj_map_.Set(packed_key, std::move(obj));
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  record.seq = next_seq;
  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }
  return WriteResult{Status::kOk, true, created, next_seq};
}
```

- [ ] **Step 4: Implement HashSlot::Update**

Updater receives `std::optional<RedisObject>` (copy of existing, or nullopt if missing/expired). Returns the new object to write, or nullopt to skip the write entirely.

**Semantics of nullopt return:**
- `nullopt` means "do not write anything to storage" (no binlog entry, no map change)
- The reason for cancellation (wrong type, no-op, etc.) is the command's responsibility to track via lambda capture
- Result.status will be `kOk` with `changed=false`; commands distinguish wrong type from normal no-op via captured state

```cpp
WriteResult HashSlot::Update(
    std::string_view key,
    std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
    BinlogRecord record,
    std::uint64_t now_us) {
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> write_lock(write_mutex_);

  ObjectMap current_map;
  std::optional<RedisObject> existing;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    current_map = redis_obj_map_;
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found != nullptr && !found->IsExpired(now_us)) {
      existing = *found;
    }
  }

  const bool created = !existing.has_value();
  std::optional<RedisObject> new_obj = updater(std::move(existing));
  if (!new_obj) {
    // Updater chose not to write. No binlog, no state change.
    return WriteResult{Status::kOk, false, false, slot_seq_};
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map = current_map.Set(packed_key, std::move(*new_obj));

  record.seq = next_seq;
  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }
  return WriteResult{Status::kOk, true, created, next_seq};
}
```

- [ ] **Step 5: Add declarations to cache_engine.h**

```cpp
// Add after constructor, before existing methods:

  std::optional<RedisObject> Get(std::string_view key, std::uint64_t now_us) const;
  WriteResult Set(std::string_view key, RedisObject obj, BinlogRecord record,
                  std::uint64_t now_us);
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
      BinlogRecord record,
      std::uint64_t now_us);
```

Add `#include <functional>` and `#include <optional>` to cache_engine.h.

- [ ] **Step 6: Implement CacheEngine generic methods**

```cpp
std::optional<RedisObject> CacheEngine::Get(std::string_view key,
                                            std::uint64_t now_us) const {
  return SlotForKey(key).Get(key, now_us);
}

WriteResult CacheEngine::Set(std::string_view key, RedisObject obj,
                             BinlogRecord record,
                             std::uint64_t now_us) {
  return SlotForKey(key).Set(key, std::move(obj), std::move(record), now_us);
}

WriteResult CacheEngine::Update(
    std::string_view key,
    std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
    BinlogRecord record,
    std::uint64_t now_us) {
  return SlotForKey(key).Update(key, std::move(updater), std::move(record),
                                now_us);
}
```

- [ ] **Step 7: Create tests/cache_engine_generic_test.cpp**

```cpp
#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"

namespace {

cache::BinlogRecord MakeRecord(cache::BinlogOp op,
                               std::vector<std::string> args) {
  cache::BinlogRecord record;
  record.op = op;
  record.args = std::move(args);
  return record;
}

}  // namespace

CACHE_TEST(GenericSetAndGet) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;

  auto r = engine.Set("k", cache::RedisObject::MakeString("v"),
                      MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}),
                      now);
  test::Require(r.status == cache::Status::kOk, "set ok");
  test::Require(r.created, "created");

  auto obj = engine.Get("k", now);
  test::Require(obj.has_value(), "get found");
  test::RequireEqual(obj->StringValue()->ToString(), std::string("v"), "value");
}

CACHE_TEST(GenericGetMissing) {
  cache::CacheEngine engine;
  auto obj = engine.Get("missing", 1'000'000);
  test::Require(!obj.has_value(), "get missing returns nullopt");
}

CACHE_TEST(GenericUpdateCreates) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;

  auto r = engine.Update("k", [](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    test::Require(!existing.has_value(), "no existing");
    return cache::RedisObject::MakeString("new");
  }, MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "new"}), now);

  test::Require(r.created, "created");
  auto obj = engine.Get("k", now);
  test::RequireEqual(obj->StringValue()->ToString(), std::string("new"), "value");
}

CACHE_TEST(GenericUpdateModifies) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;
  engine.Set("k", cache::RedisObject::MakeString("old"),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "old"}), now);

  auto r = engine.Update("k", [](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    test::Require(existing.has_value(), "has existing");
    return cache::RedisObject::MakeString("new");
  }, MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "new"}), now);

  test::Require(!r.created, "not created");
  auto obj = engine.Get("k", now);
  test::RequireEqual(obj->StringValue()->ToString(), std::string("new"), "value");
}

CACHE_TEST(GenericUpdateCancels) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;
  engine.Set("k", cache::RedisObject::MakeString("v"),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}), now);
  const std::size_t before_logs =
      engine.SlotForKey("k").CopyLogsAfter(0, 10).size();

  auto r = engine.Update("k", [](std::optional<cache::RedisObject>)
      -> std::optional<cache::RedisObject> {
    return std::nullopt;
  }, MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "ignored"}), now);

  test::Require(r.status == cache::Status::kOk, "cancelled cleanly");
  test::Require(!r.changed, "not changed");
  const std::size_t after_logs =
      engine.SlotForKey("k").CopyLogsAfter(0, 10).size();
  test::Require(after_logs == before_logs, "cancel does not append binlog");
}
```

- [ ] **Step 8: Run tests**

Run: `make test`

- [ ] **Step 9: Commit**

```bash
git add src/cache/hash_slot.h src/cache/hash_slot.cpp src/cache/cache_engine.h src/cache/cache_engine.cpp tests/cache_engine_generic_test.cpp
git commit -m "feat: add generic Get/Set/Update to HashSlot and CacheEngine"
```

---

## Task 3: Update RedisCmd Interface and CommandDispatcher

**Files:** Modify `src/command/redis_cmd.h`, `src/command/command_dispatcher.h`, `src/command/command_dispatcher.cpp`

**Rationale:** Update RedisCmd and CommandDispatcher together. The new dispatcher owns a registry of static command objects and exposes both `Execute(std::vector<std::string_view>)` and `Execute(std::vector<std::string>)`; the string overload keeps `RespCodec::CommandArgs` and `src/net/server.cpp` compatible.

- [ ] **Step 1: Update src/command/redis_cmd.h**

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "protocol/response.h"

namespace command {

class RedisCmd {
 public:
  virtual ~RedisCmd() = default;

  virtual std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const = 0;

  virtual protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const = 0;
};

}  // namespace command
```

- [ ] **Step 2: Update src/command/command_dispatcher.h**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cache/cache_engine.h"
#include "command/redis_cmd.h"

namespace command {

class CommandDispatcher {
 public:
  CommandDispatcher();

  protocol::Response Execute(const std::vector<std::string>& args,
                             cache::CacheEngine& engine,
                             std::uint64_t now_us) const;
  protocol::Response Execute(const std::vector<std::string_view>& args,
                             cache::CacheEngine& engine,
                             std::uint64_t now_us) const;

 private:
  struct CommandEntry {
    RedisCmd* cmd;
  };

  std::unordered_map<std::string, CommandEntry> commands_;
};

}  // namespace command
```

- [ ] **Step 3: Implement CommandDispatcher constructor and Execute**

```cpp
#include "command/command_dispatcher.h"

#include "command/hash_cmd.h"
#include "command/key_cmd.h"
#include "command/set_cmd.h"
#include "command/string_cmd.h"
#include "command/zset_cmd.h"
#include "common/parse_utils.h"

namespace command {

CommandDispatcher::CommandDispatcher() {
  static SetCmd set_cmd;
  static GetCmd get_cmd;
  static HSetCmd hset_cmd;
  static HGetCmd hget_cmd;
  static SAddCmd sadd_cmd;
  static SIsMemberCmd sismember_cmd;
  static ZAddCmd zadd_cmd;
  static ZScoreCmd zscore_cmd;
  static DelCmd del_cmd;
  static ExpireCmd expire_cmd;
  static TtlCmd ttl_cmd;

  commands_["SET"] = {&set_cmd};
  commands_["GET"] = {&get_cmd};
  commands_["HSET"] = {&hset_cmd};
  commands_["HGET"] = {&hget_cmd};
  commands_["SADD"] = {&sadd_cmd};
  commands_["SISMEMBER"] = {&sismember_cmd};
  commands_["ZADD"] = {&zadd_cmd};
  commands_["ZSCORE"] = {&zscore_cmd};
  commands_["DEL"] = {&del_cmd};
  commands_["EXPIRE"] = {&expire_cmd};
  commands_["TTL"] = {&ttl_cmd};
}

protocol::Response CommandDispatcher::Execute(
    const std::vector<std::string>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const std::string& arg : args) {
    views.push_back(arg);
  }
  return Execute(views, engine, now_us);
}

protocol::Response CommandDispatcher::Execute(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  if (args.empty()) {
    return protocol::Response::Error("ERR empty command");
  }

  std::string cmd_name = common::ToUpperAscii(args[0]);
  auto it = commands_.find(cmd_name);
  if (it == commands_.end()) {
    return protocol::Response::Error("ERR unknown command '" + cmd_name + "'");
  }

  auto arity_error = it->second.cmd->CheckArity(args);
  if (arity_error) {
    return protocol::Response::Error(*arity_error);
  }

  return it->second.cmd->ExecCmd(args, engine, now_us);
}

}  // namespace command
```

- [ ] **Step 4: Verify compilation**

Run: `make build`
Note: Will fail because command classes don't implement new interface yet. Proceed to Task 4-8.

- [ ] **Step 5: Commit (after Task 4-8 complete)**

Commit together with command refactoring in Task 4-8.

---

## Task 4: Refactor String Commands

**Files:** Modify `src/command/string_cmd.h`, `src/command/string_cmd.cpp`

- [ ] **Step 1: Rewrite src/command/string_cmd.h**

```cpp
#pragma once

#include "command/redis_cmd.h"

namespace command {

class SetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class GetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
```

- [ ] **Step 2: Rewrite src/command/string_cmd.cpp**

```cpp
#include "command/string_cmd.h"

#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace command {

std::optional<std::string> SetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) return "ERR wrong number of arguments for 'set' command";
  return std::nullopt;
}

protocol::Response SetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", std::string(args[1]), std::string(args[2])};
  engine.Set(args[1], cache::RedisObject::MakeString(args[2]),
             std::move(record), now_us);
  return protocol::Response::SimpleString("OK");
}

std::optional<std::string> GetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) return "ERR wrong number of arguments for 'get' command";
  return std::nullopt;
}

protocol::Response GetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) return protocol::Response::NullBulk();
  if (obj->Type() != cache::RedisObjectType::kString) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  const cache::PackedString* value = obj->StringValue();
  if (value == nullptr) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  return protocol::Response::BulkString(value->ToString());
}

}  // namespace command
```

---

## Task 5: Refactor Hash Commands

**Files:** Modify `src/command/hash_cmd.h`, `src/command/hash_cmd.cpp`

- [ ] **Step 1: Rewrite src/command/hash_cmd.h**

```cpp
#pragma once

#include "command/redis_cmd.h"

namespace command {

class HSetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class HGetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
```

- [ ] **Step 2: Rewrite src/command/hash_cmd.cpp**

```cpp
#include "command/hash_cmd.h"

#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace command {

std::optional<std::string> HSetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) return "ERR wrong number of arguments for 'hset' command";
  return std::nullopt;
}

protocol::Response HSetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  std::string_view key = args[1], field = args[2], value = args[3];
  bool created = false;
  bool wrong_type = false;
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kHSet;
  record.args = {"HSET", std::string(key), std::string(field),
                 std::string(value)};

  engine.Update(key, [&](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    cache::HashValue hash;
    std::uint64_t deadline_us = 0;
    if (existing) {
      const cache::HashValue* existing_hash = existing->Hash();
      if (existing->Type() != cache::RedisObjectType::kHash ||
          existing_hash == nullptr) {
        wrong_type = true;
        return std::nullopt;
      }
      hash = *existing_hash;
      deadline_us = existing->DeadlineUs();
      created = hash.Find(cache::PackedString(field)) == nullptr;
    } else {
      created = true;
    }
    cache::HashValue next = hash.Set(cache::PackedString(field), cache::PackedString(value));
    cache::RedisObject obj = cache::RedisObject::MakeHash(std::move(next));
    return deadline_us ? obj.WithDeadline(deadline_us) : obj;
  }, std::move(record), now_us);

  if (wrong_type) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  return protocol::Response::Integer(created ? 1 : 0);
}

std::optional<std::string> HGetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) return "ERR wrong number of arguments for 'hget' command";
  return std::nullopt;
}

protocol::Response HGetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) return protocol::Response::NullBulk();
  if (obj->Type() != cache::RedisObjectType::kHash) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  const cache::HashValue* hash = obj->Hash();
  if (hash == nullptr) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  const cache::PackedString* found = hash->Find(cache::PackedString(args[2]));
  if (!found) return protocol::Response::NullBulk();
  return protocol::Response::BulkString(found->ToString());
}

}  // namespace command
```

---

## Task 6: Refactor Set Commands

**Files:** Modify `src/command/set_cmd.h`, `src/command/set_cmd.cpp`

- [ ] **Step 1: Rewrite set_cmd.h**

```cpp
#pragma once

#include "command/redis_cmd.h"

namespace command {

class SAddCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class SIsMemberCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
```

- [ ] **Step 2: Rewrite set_cmd.cpp**

```cpp
#include "command/set_cmd.h"

#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace command {

std::optional<std::string> SAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) return "ERR wrong number of arguments for 'sadd' command";
  return std::nullopt;
}

protocol::Response SAddCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  std::string_view key = args[1], member = args[2];
  bool added = false;
  bool wrong_type = false;
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSAdd;
  record.args = {"SADD", std::string(key), std::string(member)};

  engine.Update(key, [&](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    cache::SetValue set;
    std::uint64_t deadline_us = 0;
    if (existing) {
      const cache::SetValue* existing_set = existing->Set();
      if (existing->Type() != cache::RedisObjectType::kSet ||
          existing_set == nullptr) {
        wrong_type = true;
        return std::nullopt;
      }
      set = *existing_set;
      deadline_us = existing->DeadlineUs();
      if (set.Contains(cache::PackedString(member))) {
        // Member already present: skip the write to avoid binlog noise.
        return std::nullopt;
      }
    }
    added = true;
    cache::SetValue next = set.Add(cache::PackedString(member));
    cache::RedisObject obj = cache::RedisObject::MakeSet(std::move(next));
    return deadline_us ? obj.WithDeadline(deadline_us) : obj;
  }, std::move(record), now_us);

  if (wrong_type) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  return protocol::Response::Integer(added ? 1 : 0);
}

std::optional<std::string> SIsMemberCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) return "ERR wrong number of arguments for 'sismember' command";
  return std::nullopt;
}

protocol::Response SIsMemberCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) return protocol::Response::Integer(0);
  if (obj->Type() != cache::RedisObjectType::kSet) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  const cache::SetValue* set = obj->Set();
  if (set == nullptr) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  bool is_member = set->Contains(cache::PackedString(args[2]));
  return protocol::Response::Integer(is_member ? 1 : 0);
}

}  // namespace command
```

---

## Task 7: Refactor ZSet Commands

**Files:** Modify `src/command/zset_cmd.h`, `src/command/zset_cmd.cpp`

- [ ] **Step 1: Rewrite zset_cmd.h**

```cpp
#pragma once

#include "command/redis_cmd.h"

namespace command {

class ZAddCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class ZScoreCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
```

- [ ] **Step 2: Rewrite zset_cmd.cpp**

Note: keep the existing `FormatScore` helper to match Redis-compatible output (e.g., `"3.14"` not `"3.140000"`). Existing integration tests assume this format.

```cpp
#include "command/zset_cmd.h"

#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"
#include "common/parse_utils.h"

namespace command {
namespace {

std::string FormatScore(double score) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << score;
  std::string text = out.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  return text;
}

}  // namespace

std::optional<std::string> ZAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) return "ERR wrong number of arguments for 'zadd' command";
  return std::nullopt;
}

protocol::Response ZAddCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  double score = 0.0;
  if (!common::ParseFiniteDouble(args[2], &score)) {
    return protocol::Response::Error("ERR value is not a valid float");
  }

  std::string_view key = args[1], member = args[3];
  bool created = false;
  bool wrong_type = false;
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kZAdd;
  record.args = {"ZADD", std::string(key), FormatScore(score),
                 std::string(member)};

  engine.Update(key, [&](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    cache::ZSetValue zset;
    std::uint64_t deadline_us = 0;
    if (existing) {
      const cache::ZSetValue* existing_zset = existing->ZSet();
      if (existing->Type() != cache::RedisObjectType::kZSet ||
          existing_zset == nullptr) {
        wrong_type = true;
        return std::nullopt;
      }
      zset = *existing_zset;
      deadline_us = existing->DeadlineUs();
      created = zset.Find(cache::PackedString(member)) == nullptr;
    } else {
      created = true;
    }
    cache::ZSetValue next = zset.Set(cache::PackedString(member), score);
    cache::RedisObject obj = cache::RedisObject::MakeZSet(std::move(next));
    return deadline_us ? obj.WithDeadline(deadline_us) : obj;
  }, std::move(record), now_us);

  if (wrong_type) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  return protocol::Response::Integer(created ? 1 : 0);
}

std::optional<std::string> ZScoreCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) return "ERR wrong number of arguments for 'zscore' command";
  return std::nullopt;
}

protocol::Response ZScoreCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) return protocol::Response::NullBulk();
  if (obj->Type() != cache::RedisObjectType::kZSet) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  const cache::ZSetValue* zset = obj->ZSet();
  if (zset == nullptr) {
    return protocol::Response::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
  }
  const double* found = zset->Find(cache::PackedString(args[2]));
  if (!found) return protocol::Response::NullBulk();
  return protocol::Response::BulkString(FormatScore(*found));
}

}  // namespace command
```

---

## Task 8: Refactor Key Commands

**Files:** Modify `src/command/key_cmd.h`, `src/command/key_cmd.cpp`

- [ ] **Step 1: Rewrite key_cmd.h**

```cpp
#pragma once

#include "command/redis_cmd.h"

namespace command {

class DelCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class ExpireCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class TtlCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
```

- [ ] **Step 2: Rewrite key_cmd.cpp**

```cpp
#include "command/key_cmd.h"
#include "common/parse_utils.h"

namespace command {

std::optional<std::string> DelCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) return "ERR wrong number of arguments for 'del' command";
  return std::nullopt;
}

protocol::Response DelCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  auto result = engine.Del(args[1], now_us);
  return protocol::Response::Integer(result.changed ? 1 : 0);
}

std::optional<std::string> ExpireCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) return "ERR wrong number of arguments for 'expire' command";
  return std::nullopt;
}

protocol::Response ExpireCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  std::int64_t seconds = 0;
  if (!common::ParseInt64(args[2], &seconds)) {
    return protocol::Response::Error("ERR value is not an integer or out of range");
  }
  bool ok = engine.Expire(args[1], seconds, now_us);
  return protocol::Response::Integer(ok ? 1 : 0);
}

std::optional<std::string> TtlCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) return "ERR wrong number of arguments for 'ttl' command";
  return std::nullopt;
}

protocol::Response TtlCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine, std::uint64_t now_us) const {
  return protocol::Response::Integer(engine.Ttl(args[1], now_us));
}

}  // namespace command
```

---

## Task 9: Commit All Command Changes and Update Tests

- [ ] **Step 1: Rewrite tests/cache_command_test.cpp**

```cpp
#include "test_harness.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/response.h"

namespace {

void RequireType(const protocol::Response& response,
                 protocol::ResponseType expected, std::string_view message) {
  test::Require(response.type == expected, message);
}

protocol::Response Exec(command::CommandDispatcher& dispatcher,
                        cache::CacheEngine& engine,
                        std::uint64_t now_us,
                        std::initializer_list<std::string_view> args) {
  std::vector<std::string> owned_args;
  owned_args.reserve(args.size());
  for (std::string_view arg : args) {
    owned_args.emplace_back(arg);
  }
  return dispatcher.Execute(owned_args, engine, now_us);
}

}  // namespace

CACHE_TEST(StringAndKeyCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  auto set_response = Exec(dispatcher, engine, now_us, {"SET", "k", "v"});
  RequireType(set_response, protocol::ResponseType::kSimpleString,
              "SET returns a simple string");
  test::RequireEqual(set_response.text, "OK", "SET returns OK");

  auto get_response = Exec(dispatcher, engine, now_us, {"GET", "k"});
  RequireType(get_response, protocol::ResponseType::kBulkString,
              "GET returns a bulk string");
  test::RequireEqual(get_response.text, "v", "GET returns value");

  auto ttl_no_expire_response = Exec(dispatcher, engine, now_us, {"TTL", "k"});
  RequireType(ttl_no_expire_response, protocol::ResponseType::kInteger,
              "TTL returns an integer");
  test::Require(ttl_no_expire_response.integer == -1,
                "TTL without expire returns -1");

  auto ttl_missing_response =
      Exec(dispatcher, engine, now_us, {"TTL", "missing"});
  RequireType(ttl_missing_response, protocol::ResponseType::kInteger,
              "TTL missing key returns an integer");
  test::Require(ttl_missing_response.integer == -2,
                "TTL missing key returns -2");

  auto expire_response = Exec(dispatcher, engine, now_us, {"EXPIRE", "k", "2"});
  RequireType(expire_response, protocol::ResponseType::kInteger,
              "EXPIRE returns an integer");
  test::Require(expire_response.integer == 1, "EXPIRE existing key returns 1");

  auto ttl_response = Exec(dispatcher, engine, now_us + 500'000, {"TTL", "k"});
  RequireType(ttl_response, protocol::ResponseType::kInteger,
              "TTL with expire returns an integer");
  test::Require(ttl_response.integer == 1, "TTL floors remaining seconds");

  auto expired_get_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"GET", "k"});
  RequireType(expired_get_response, protocol::ResponseType::kNullBulkString,
              "GET expired key returns null bulk");

  auto reset_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"SET", "k", "v2"});
  RequireType(reset_response, protocol::ResponseType::kSimpleString,
              "SET after expire returns a simple string");
  test::Require(Exec(dispatcher, engine, now_us + 2'000'000, {"TTL", "k"})
                    .integer == -1,
                "SET clears prior TTL");

  auto delete_now_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"EXPIRE", "k", "0"});
  RequireType(delete_now_response, protocol::ResponseType::kInteger,
              "EXPIRE <= 0 returns an integer");
  test::Require(delete_now_response.integer == 1,
                "EXPIRE <= 0 deletes existing key");
  RequireType(Exec(dispatcher, engine, now_us + 2'000'000, {"GET", "k"}),
              protocol::ResponseType::kNullBulkString,
              "GET immediately deleted key returns null bulk");

  auto missing_del_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"DEL", "k"});
  RequireType(missing_del_response, protocol::ResponseType::kInteger,
              "DEL missing key returns an integer");
  test::Require(missing_del_response.integer == 0, "DEL missing key returns 0");

  (void)Exec(dispatcher, engine, now_us + 2'000'000, {"SET", "k", "v3"});
  auto del_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"DEL", "k"});
  RequireType(del_response, protocol::ResponseType::kInteger,
              "DEL returns an integer");
  test::Require(del_response.integer == 1, "DEL removes key");
}

CACHE_TEST(ExpireUsesSaturatedTtlInBinlog) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 0;

  (void)Exec(dispatcher, engine, now_us, {"SET", "overflow", "v"});

  const std::string max_int =
      std::to_string(std::numeric_limits<std::int64_t>::max());
  auto expire_response =
      Exec(dispatcher, engine, now_us, {"EXPIRE", "overflow", max_int});
  RequireType(expire_response, protocol::ResponseType::kInteger,
              "overflow EXPIRE returns an integer");
  test::Require(expire_response.integer == 1,
                "overflow EXPIRE succeeds for existing key");

  auto get_response = Exec(dispatcher, engine, now_us, {"GET", "overflow"});
  RequireType(get_response, protocol::ResponseType::kBulkString,
              "saturated EXPIRE keeps key readable before deadline");
  test::RequireEqual(get_response.text, "v", "saturated EXPIRE keeps value");

  auto logs = engine.SlotForKey("overflow").CopyLogsAfter(0, 10);
  test::Require(logs.size() == 2, "SET and EXPIRE logs are present");
  test::Require(logs[1].op == cache::BinlogOp::kExpire,
                "second log records EXPIRE");
  test::Require(logs[1].remaining_ttl_us ==
                    std::numeric_limits<std::uint64_t>::max(),
                "EXPIRE log stores saturated remaining TTL");
}

CACHE_TEST(HashSetAndZSetCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 10'000;

  test::Require(Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v"})
                    .integer == 1,
                "HSET new field returns 1");
  test::Require(Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v2"})
                    .integer == 0,
                "HSET existing field returns 0");
  test::RequireEqual(
      Exec(dispatcher, engine, now_us, {"HGET", "h", "f"}).text, "v2",
      "HGET returns updated value");

  test::Require(Exec(dispatcher, engine, now_us, {"SADD", "s", "m"}).integer ==
                    1,
                "SADD new member returns 1");
  test::Require(Exec(dispatcher, engine, now_us, {"SADD", "s", "m"}).integer ==
                    0,
                "SADD existing member returns 0");
  test::Require(Exec(dispatcher, engine, now_us, {"SISMEMBER", "s", "m"})
                    .integer == 1,
                "SISMEMBER returns 1");

  test::Require(Exec(dispatcher, engine, now_us, {"ZADD", "z", "1.5", "m"})
                    .integer == 1,
                "ZADD new member returns 1");
  test::Require(Exec(dispatcher, engine, now_us, {"ZADD", "z", "2.5", "m"})
                    .integer == 0,
                "ZADD existing member returns 0");
  test::RequireEqual(
      Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "m"}).text, "2.5",
      "ZSCORE returns score");
}

CACHE_TEST(CommandDispatcherParsesAndExecutesRespArgs) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1000;

  auto set = Exec(dispatcher, engine, now_us, {"set", "a", "1"});
  test::RequireEqual(set.text, "OK", "dispatcher executes SET");

  auto get = Exec(dispatcher, engine, now_us, {"GET", "a"});
  test::RequireEqual(get.text, "1", "dispatcher executes GET");

  std::vector<std::string_view> get_views = {"GET", "a"};
  auto get_view = dispatcher.Execute(get_views, engine, now_us);
  test::RequireEqual(get_view.text, "1", "string_view overload executes GET");

  auto hset = Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v"});
  test::Require(hset.integer == 1, "dispatcher executes HSET");
  auto hget = Exec(dispatcher, engine, now_us, {"hget", "h", "f"});
  test::RequireEqual(hget.text, "v", "dispatcher executes HGET");

  auto sadd = Exec(dispatcher, engine, now_us, {"SADD", "s", "m"});
  test::Require(sadd.integer == 1, "dispatcher executes SADD");
  auto sismember = Exec(dispatcher, engine, now_us, {"SISMEMBER", "s", "m"});
  test::Require(sismember.integer == 1, "dispatcher executes SISMEMBER");

  auto zadd = Exec(dispatcher, engine, now_us, {"ZADD", "z", "1.5", "m"});
  test::Require(zadd.integer == 1, "dispatcher executes ZADD");
  auto zscore = Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "m"});
  test::RequireEqual(zscore.text, "1.5", "dispatcher executes ZSCORE");

  auto expire = Exec(dispatcher, engine, now_us, {"EXPIRE", "a", "2"});
  test::Require(expire.integer == 1, "dispatcher parses EXPIRE integer");
  auto ttl = Exec(dispatcher, engine, now_us, {"TTL", "a"});
  test::Require(ttl.integer == 2, "dispatcher executes TTL");
  auto del = Exec(dispatcher, engine, now_us, {"DEL", "a"});
  test::Require(del.integer == 1, "dispatcher executes DEL");

  auto wrong_arity = Exec(dispatcher, engine, now_us, {"GET"});
  test::Require(wrong_arity.type == protocol::ResponseType::kError,
                "wrong arity returns error");

  auto invalid_score =
      Exec(dispatcher, engine, now_us, {"ZADD", "z", "nan", "m"});
  test::Require(invalid_score.type == protocol::ResponseType::kError,
                "invalid score returns error");

  auto invalid_integer =
      Exec(dispatcher, engine, now_us, {"EXPIRE", "a", "oops"});
  test::Require(invalid_integer.type == protocol::ResponseType::kError,
                "invalid integer returns error");

  auto unknown = Exec(dispatcher, engine, now_us, {"NOPE", "a"});
  test::Require(unknown.type == protocol::ResponseType::kError,
                "unknown command returns error");
}
```

- [ ] **Step 2: Verify compilation**

Run: `make build`

- [ ] **Step 3: Run all tests**

Run: `make test`

- [ ] **Step 4: Commit all changes**

```bash
git add src/command/ tests/cache_command_test.cpp
git commit -m "refactor: stateless commands with registry-based dispatch

- RedisCmd: add CheckArity, ExecCmd accepts args vector
- CommandDispatcher: O(1) map lookup, static command instances
- All commands: stateless, use CacheEngine generic Get/Set/Update
- Tests: exercise commands only through CommandDispatcher"
```

---

## Task 10: Migrate Replication and Remaining Tests off Old CacheEngine Helpers

**Files:** Modify `src/repl/slave_replicator.cpp`, `tests/repl_test.cpp`, `tests/expire_test.cpp`

- [ ] **Step 1: Update src/repl/slave_replicator.cpp includes and helper**

Add the new includes:

```cpp
#include <optional>
#include <utility>

#include "cache/redis_object.h"
```

Add this helper inside the anonymous namespace:

```cpp
cache::RedisObject PreserveDeadline(cache::RedisObject object,
                                    std::uint64_t deadline_us) {
  if (deadline_us == 0) {
    return object;
  }
  return object.WithDeadline(deadline_us);
}
```

- [ ] **Step 2: Replace SlaveReplicator::ApplyRecord**

```cpp
bool SlaveReplicator::ApplyRecord(const cache::BinlogRecord& record,
                                  std::uint64_t now_us) {
  if (engine_ == nullptr) {
    return false;
  }

  switch (record.op) {
    case cache::BinlogOp::kSet:
      if (record.args.size() == 3) {
        cache::BinlogRecord replay_record = record;
        engine_->Set(record.args[1],
                     cache::RedisObject::MakeString(record.args[2]),
                     std::move(replay_record), now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kDel:
      if (record.args.size() == 2) {
        engine_->Del(record.args[1], now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kExpire:
      if (record.args.size() == 2 || record.args.size() == 3) {
        std::int64_t seconds = 0;
        if (!RelativeExpireSeconds(record, &seconds)) {
          return false;
        }
        engine_->Expire(record.args[1], seconds, now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kHSet:
      if (record.args.size() == 4) {
        cache::BinlogRecord replay_record = record;
        engine_->Update(
            record.args[1],
            [&](std::optional<cache::RedisObject> existing)
                -> std::optional<cache::RedisObject> {
              cache::HashValue hash;
              std::uint64_t deadline_us = 0;
              if (existing) {
                const cache::HashValue* existing_hash = existing->Hash();
                if (existing->Type() != cache::RedisObjectType::kHash ||
                    existing_hash == nullptr) {
                  return std::nullopt;
                }
                hash = *existing_hash;
                deadline_us = existing->DeadlineUs();
              }
              cache::HashValue next = hash.Set(
                  cache::PackedString(record.args[2]),
                  cache::PackedString(record.args[3]));
              return PreserveDeadline(
                  cache::RedisObject::MakeHash(std::move(next)), deadline_us);
            },
            std::move(replay_record), now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kSAdd:
      if (record.args.size() == 3) {
        cache::BinlogRecord replay_record = record;
        engine_->Update(
            record.args[1],
            [&](std::optional<cache::RedisObject> existing)
                -> std::optional<cache::RedisObject> {
              cache::SetValue set;
              std::uint64_t deadline_us = 0;
              const cache::PackedString member(record.args[2]);
              if (existing) {
                const cache::SetValue* existing_set = existing->Set();
                if (existing->Type() != cache::RedisObjectType::kSet ||
                    existing_set == nullptr) {
                  return std::nullopt;
                }
                set = *existing_set;
                deadline_us = existing->DeadlineUs();
                if (set.Contains(member)) {
                  return std::nullopt;
                }
              }
              cache::SetValue next = set.Add(member);
              return PreserveDeadline(
                  cache::RedisObject::MakeSet(std::move(next)), deadline_us);
            },
            std::move(replay_record), now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kZAdd:
      if (record.args.size() == 4) {
        double score = 0.0;
        if (!ParseDouble(record.args[2], &score)) {
          return false;
        }
        cache::BinlogRecord replay_record = record;
        engine_->Update(
            record.args[1],
            [&](std::optional<cache::RedisObject> existing)
                -> std::optional<cache::RedisObject> {
              cache::ZSetValue zset;
              std::uint64_t deadline_us = 0;
              if (existing) {
                const cache::ZSetValue* existing_zset = existing->ZSet();
                if (existing->Type() != cache::RedisObjectType::kZSet ||
                    existing_zset == nullptr) {
                  return std::nullopt;
                }
                zset = *existing_zset;
                deadline_us = existing->DeadlineUs();
              }
              cache::ZSetValue next =
                  zset.Set(cache::PackedString(record.args[3]), score);
              return PreserveDeadline(
                  cache::RedisObject::MakeZSet(std::move(next)), deadline_us);
            },
            std::move(replay_record), now_us);
        return true;
      }
      break;
  }

  return false;
}
```

- [ ] **Step 3: Rewrite tests/repl_test.cpp**

```cpp
#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cache/cache_engine.h"
#include "cache/redis_object.h"
#include "common/hash.h"
#include "redis/resp.h"
#include "repl/master_replicator.h"
#include "repl/repl_frame.h"
#include "repl/slave_replicator.h"

namespace {

cache::BinlogRecord MakeRecord(std::uint64_t seq, cache::BinlogOp op,
                               std::vector<std::string> args) {
  cache::BinlogRecord record;
  record.seq = seq;
  record.op = op;
  record.args = std::move(args);
  return record;
}

void WriteString(cache::CacheEngine& engine, std::string_view key,
                 std::string_view value, std::uint64_t now_us) {
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", std::string(key), std::string(value)};
  engine.Set(key, cache::RedisObject::MakeString(value), std::move(record),
             now_us);
}

void RequireString(cache::CacheEngine& engine, std::string_view key,
                   std::uint64_t now_us, std::string_view expected) {
  auto obj = engine.Get(key, now_us);
  test::Require(obj.has_value(), "string key exists");
  test::Require(obj->Type() == cache::RedisObjectType::kString,
                "object is string");
  const cache::PackedString* value = obj->StringValue();
  test::Require(value != nullptr, "string value pointer exists");
  test::RequireEqual(value->ToString(), expected, "string value");
}

}  // namespace

CACHE_TEST(ReplFrameRoundTripsLog) {
  cache::BinlogRecord record;
  record.seq = 7;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", "k", "v"};

  repl::Frame frame = repl::Frame::Log(3, record);
  std::string wire = repl::EncodeFrame(frame);
  auto decoded = repl::DecodeFrame(wire);

  test::Require(decoded.has_value(), "frame decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kLog, "decoded LOG");
  test::Require(decoded->slot_id == 3, "slot id round trips");
  test::Require(decoded->record.seq == 7, "seq round trips");
}

CACHE_TEST(MasterReplicatorUsesAckToCleanLogs) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  WriteString(engine, "k", "v2", 200);

  repl::MasterReplicator repl(&engine);
  repl.OnAck(common::SlotForKey("k"), 2);

  auto records = engine.SlotForKey("k").CopyLogsAfter(0, 10);
  test::Require(records.empty(), "acked logs are cleaned");
}

CACHE_TEST(ReplFrameRejectsMalformedAckCount) {
  std::string wire;
  redis::PackArrayHeader(3, &wire);
  redis::PackBulkString("CACHE.REPL", &wire);
  redis::PackBulkString("ACK", &wire);
  redis::PackBulkString("999999999999999999", &wire);

  test::Require(!repl::DecodeFrame(wire).has_value(),
                "malformed ACK count is rejected");
}

CACHE_TEST(SlaveApplyHoldsOutOfOrderLogsUntilGapFilled) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord seq2 =
      MakeRecord(2, cache::BinlogOp::kSet, {"SET", "k", "v2"});
  cache::BinlogRecord seq1 =
      MakeRecord(1, cache::BinlogOp::kSet, {"SET", "k", "v1"});

  const std::size_t slot = common::SlotForKey("k");
  test::Require(slave.WorkerForSlotForTest(slot) == slot % 4,
                "slot is routed to deterministic apply worker");
  slave.ApplyLogForTest(slot, seq2, 1000);
  test::Require(!engine.Get("k", 1000).has_value(), "seq2 waits for seq1");

  slave.ApplyLogForTest(slot, seq1, 1000);
  RequireString(engine, "k", 1000, "v2");
  test::Require(slave.AppliedSeqForTest(slot) == 2, "applied seq advances");
}

CACHE_TEST(SlaveApplyDoesNotAdvanceSeqForMalformedLog) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord malformed =
      MakeRecord(1, cache::BinlogOp::kSet, {"SET", "k"});

  const std::size_t slot = common::SlotForKey("k");
  slave.ApplyLogForTest(slot, malformed, 1000);

  test::Require(slave.AppliedSeqForTest(slot) == 0,
                "malformed log does not advance seq");
  test::Require(!engine.Get("k", 1000).has_value(),
                "malformed log does not mutate data");
}

CACHE_TEST(SlaveApplySaturatedExpireDoesNotDeleteKey) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord set =
      MakeRecord(1, cache::BinlogOp::kSet, {"SET", "ttl", "v"});

  cache::BinlogRecord expire;
  expire.seq = 2;
  expire.op = cache::BinlogOp::kExpire;
  expire.args = {"EXPIRE", "ttl", "1"};
  expire.remaining_ttl_us = std::numeric_limits<std::uint64_t>::max();

  const std::size_t slot = common::SlotForKey("ttl");
  slave.ApplyLogForTest(slot, set, 1000);
  slave.ApplyLogForTest(slot, expire, 1000);

  RequireString(engine, "ttl", 1000, "v");
  test::Require(slave.AppliedSeqForTest(slot) == 2,
                "saturated expire advances seq");
}

CACHE_TEST(SlaveApplyReplaysGenericCommandTypes) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);
  const std::uint64_t now_us = 10'000;
  std::map<std::size_t, std::uint64_t> next_seq_by_slot;

  auto apply = [&](cache::BinlogOp op, std::vector<std::string> args) {
    const std::size_t slot = common::SlotForKey(args[1]);
    const std::uint64_t seq = ++next_seq_by_slot[slot];
    cache::BinlogRecord record = MakeRecord(seq, op, std::move(args));
    slave.ApplyLogForTest(slot, record, now_us);
    test::Require(slave.AppliedSeqForTest(slot) == record.seq,
                  "applied seq advances for record");
  };

  apply(cache::BinlogOp::kHSet, {"HSET", "h", "f", "v"});
  auto hash = engine.Get("h", now_us);
  test::Require(hash.has_value(), "hash key exists");
  test::Require(hash->Type() == cache::RedisObjectType::kHash,
                "hash type replays");
  const cache::HashValue* hash_map = hash->Hash();
  test::Require(hash_map != nullptr, "hash pointer exists");
  const cache::PackedString* hash_value = hash_map->Find(cache::PackedString("f"));
  test::Require(hash_value != nullptr, "hash field exists");
  test::RequireEqual(hash_value->ToString(), "v", "hash field value");

  apply(cache::BinlogOp::kSAdd, {"SADD", "s", "m"});
  auto set = engine.Get("s", now_us);
  test::Require(set.has_value(), "set key exists");
  test::Require(set->Type() == cache::RedisObjectType::kSet,
                "set type replays");
  const cache::SetValue* set_value = set->Set();
  test::Require(set_value != nullptr, "set pointer exists");
  test::Require(set_value->Contains(cache::PackedString("m")), "set member exists");

  apply(cache::BinlogOp::kZAdd, {"ZADD", "z", "1.5", "m"});
  auto zset = engine.Get("z", now_us);
  test::Require(zset.has_value(), "zset key exists");
  test::Require(zset->Type() == cache::RedisObjectType::kZSet,
                "zset type replays");
  const cache::ZSetValue* zset_value = zset->ZSet();
  test::Require(zset_value != nullptr, "zset pointer exists");
  const double* score = zset_value->Find(cache::PackedString("m"));
  test::Require(score != nullptr && *score == 1.5, "zset score replays");

  apply(cache::BinlogOp::kSet, {"SET", "gone", "v"});
  apply(cache::BinlogOp::kDel, {"DEL", "gone"});
  test::Require(!engine.Get("gone", now_us).has_value(), "DEL replays");
}
```

- [ ] **Step 4: Rewrite tests/expire_test.cpp**

```cpp
#include "test_harness.h"

#include <cstdint>
#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"
#include "common/hash.h"
#include "expire/expire_sweeper.h"

CACHE_TEST(ExpireSweeperDeletesExpiredKeysViaWritePath) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;

  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", "b7p", "v"};
  engine.Set("b7p", cache::RedisObject::MakeString("v"), std::move(record),
             now_us);

  test::Require(engine.Expire("b7p", 1, now_us), "expire succeeds");
  test::Require(common::SlotForKey("b7p") == 0, "test key is in first slot");

  expire::ExpireSweeper sweeper(&engine, 8);
  std::size_t deleted = sweeper.SweepOnce(now_us + 2'000'000);
  test::Require(deleted == 1, "one expired key deleted");
  test::Require(!engine.Get("b7p", now_us + 2'000'000).has_value(),
                "expired key is gone");
}
```

- [ ] **Step 5: Run tests**

Run: `make test`

- [ ] **Step 6: Commit**

```bash
git add src/repl/slave_replicator.cpp tests/repl_test.cpp tests/expire_test.cpp
git commit -m "refactor: replay binlog through generic cache interface"
```

---

## Task 11: Remove Old CacheEngine Methods

**Files:** Modify `src/cache/cache_engine.h`, `src/cache/cache_engine.cpp`

- [ ] **Step 1: Verify no non-CacheEngine call sites remain**

Run:

```bash
rg -n "engine\.(GetString|SetString|HSet|HGet|SAdd|SIsMember|ZAdd|ZScore)|engine_->(SetString|HSet|SAdd|ZAdd)|CacheEngine::(GetString|SetString|HSet|HGet|SAdd|SIsMember|ZAdd|ZScore)" src tests
```

Expected: only `CacheEngine::...` definitions in `src/cache/cache_engine.cpp` and declarations in `src/cache/cache_engine.h` remain. No `engine.` or `engine_->` call sites should remain.

- [ ] **Step 2: Remove old method declarations from cache_engine.h**

Delete: GetString, SetString, HSet, HGet, SAdd, SIsMember, ZAdd, ZScore

- [ ] **Step 3: Remove old method implementations from cache_engine.cpp**

- [ ] **Step 4: Run tests**

Run: `make test`

- [ ] **Step 5: Commit**

```bash
git add src/cache/cache_engine.h src/cache/cache_engine.cpp
git commit -m "refactor: remove old command-specific CacheEngine methods"
```

---

## Task 12: Performance Validation

- [ ] **Step 1:** Run `make bench`
- [ ] **Step 2:** Verify performance is equal or better than baseline

---

## Completion Checklist

- [ ] All 11 commands refactored to stateless
- [ ] All tests passing
- [ ] No old CacheEngine command-specific methods remain
- [ ] `src/repl/slave_replicator.cpp` replays SET/HSET/SADD/ZADD via generic CacheEngine APIs
- [ ] Performance validated
- [ ] Code compiles without warnings
