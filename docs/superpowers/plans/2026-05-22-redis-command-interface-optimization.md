# Redis Command Interface Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor Redis command execution to use stateless, reusable command objects with O(1) dispatch and generic CacheEngine interface

**Architecture:** Three-layer design with command layer (stateless commands, validation), storage layer (generic RedisObject operations), and utility layer (shared parsing functions). Commands registered in map at startup, eliminating per-request allocations.

**Tech Stack:** C++17, CMake, CTest, existing immutable container library

**Design correction:** CacheEngine::Get returns `std::optional<RedisObject>` (copy) instead of `RedisObject*`, because HashSlot uses immutable containers — any concurrent write creates a new map, invalidating internal pointers. The existing code already copies objects out under lock (see HashSlot::GetString pattern).

---

## File Structure

**New Files:**
- `src/common/parse_utils.h` - Parsing utilities
- `src/common/parse_utils.cpp` - Implementation
- `tests/parse_utils_test.cpp` - Tests

**Modified Files:**
- `src/cache/cache_engine.h` - Replace command-specific methods with generic Get/Set/Update
- `src/cache/cache_engine.cpp` - Implement generic methods (delegate to HashSlot)
- `src/cache/hash_slot.h` - Add generic Get/Set/Update methods
- `src/cache/hash_slot.cpp` - Implement generic methods
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

CACHE_TEST(ParseInt64Valid) {
  std::int64_t v = 0;
  test::Require(common::ParseInt64("123", &v), "positive");
  test::RequireEqual(v, std::int64_t{123}, "value 123");
  test::Require(common::ParseInt64("-456", &v), "negative");
  test::RequireEqual(v, std::int64_t{-456}, "value -456");
  test::Require(common::ParseInt64("0", &v), "zero");
  test::RequireEqual(v, std::int64_t{0}, "value 0");
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
  WriteResult Set(std::string_view key, RedisObject obj, std::uint64_t now_us);
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
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
                          std::uint64_t now_us) {
  (void)now_us;
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map;
  bool created = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    created = redis_obj_map_.Find(packed_key) == nullptr;
    next_map = redis_obj_map_.Set(packed_key, std::move(obj));
  }

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kSet;
  record.args = {"SET", std::string(key)};
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
- Result.status will be `kInvalidArgument` to signal "no write happened"; commands distinguish reasons via captured state

```cpp
WriteResult HashSlot::Update(
    std::string_view key,
    std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
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
    return WriteResult{Status::kInvalidArgument, false, false, slot_seq_};
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map = current_map.Set(packed_key, std::move(*new_obj));

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kSet;
  record.args = {"SET", std::string(key)};
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
  WriteResult Set(std::string_view key, RedisObject obj, std::uint64_t now_us);
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
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
                             std::uint64_t now_us) {
  return SlotForKey(key).Set(key, std::move(obj), now_us);
}

WriteResult CacheEngine::Update(
    std::string_view key,
    std::function<std::optional<RedisObject>(std::optional<RedisObject>)> updater,
    std::uint64_t now_us) {
  return SlotForKey(key).Update(key, std::move(updater), now_us);
}
```

- [ ] **Step 7: Create tests/cache_engine_generic_test.cpp**

```cpp
#include "test_harness.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"

CACHE_TEST(GenericSetAndGet) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;

  auto r = engine.Set("k", cache::RedisObject::MakeString("v"), now);
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
  }, now);

  test::Require(r.created, "created");
  auto obj = engine.Get("k", now);
  test::RequireEqual(obj->StringValue()->ToString(), std::string("new"), "value");
}

CACHE_TEST(GenericUpdateModifies) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;
  engine.Set("k", cache::RedisObject::MakeString("old"), now);

  auto r = engine.Update("k", [](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    test::Require(existing.has_value(), "has existing");
    return cache::RedisObject::MakeString("new");
  }, now);

  test::Require(!r.created, "not created");
  auto obj = engine.Get("k", now);
  test::RequireEqual(obj->StringValue()->ToString(), std::string("new"), "value");
}

CACHE_TEST(GenericUpdateCancels) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;
  engine.Set("k", cache::RedisObject::MakeString("v"), now);

  auto r = engine.Update("k", [](std::optional<cache::RedisObject>)
      -> std::optional<cache::RedisObject> {
    return std::nullopt;
  }, now);

  test::Require(r.status == cache::Status::kInvalidArgument, "cancelled");
  test::Require(!r.changed, "not changed");
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

**Rationale:** Update both together. Keep old Build method temporarily so existing commands still compile. New Execute(string_view) method coexists with old Execute(string) during migration.

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
  engine.Set(args[1], cache::RedisObject::MakeString(args[2]), now_us);
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
  return protocol::Response::BulkString(obj->StringValue()->ToString());
}

}  // namespace command
```

---

## Task 5: Refactor Hash Commands

**Files:** Modify `src/command/hash_cmd.h`, `src/command/hash_cmd.cpp`

- [ ] **Step 1: Rewrite src/command/hash_cmd.h**

Same pattern as string_cmd.h: two classes HSetCmd and HGetCmd, no member variables.

- [ ] **Step 2: Rewrite src/command/hash_cmd.cpp**

```cpp
#include "command/hash_cmd.h"
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

  engine.Update(key, [&](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    cache::HashValue hash;
    std::uint64_t deadline_us = 0;
    if (existing) {
      if (existing->Type() != cache::RedisObjectType::kHash) {
        wrong_type = true;
        return std::nullopt;
      }
      hash = *existing->Hash();
      deadline_us = existing->DeadlineUs();
      created = hash.Find(cache::PackedString(field)) == nullptr;
    } else {
      created = true;
    }
    cache::HashValue next = hash.Set(cache::PackedString(field), cache::PackedString(value));
    cache::RedisObject obj = cache::RedisObject::MakeHash(std::move(next));
    return deadline_us ? obj.WithDeadline(deadline_us) : obj;
  }, now_us);

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
  const cache::PackedString* found = obj->Hash()->Find(cache::PackedString(args[2]));
  if (!found) return protocol::Response::NullBulk();
  return protocol::Response::BulkString(found->ToString());
}

}  // namespace command
```

---

## Task 6: Refactor Set Commands

**Files:** Modify `src/command/set_cmd.h`, `src/command/set_cmd.cpp`

- [ ] **Step 1: Rewrite set_cmd.h** (same pattern, SAddCmd + SIsMemberCmd)

- [ ] **Step 2: Rewrite set_cmd.cpp**

```cpp
#include "command/set_cmd.h"
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

  engine.Update(key, [&](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    cache::SetValue set;
    std::uint64_t deadline_us = 0;
    if (existing) {
      if (existing->Type() != cache::RedisObjectType::kSet) {
        wrong_type = true;
        return std::nullopt;
      }
      set = *existing->Set();
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
  }, now_us);

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
  bool is_member = obj->Set()->Contains(cache::PackedString(args[2]));
  return protocol::Response::Integer(is_member ? 1 : 0);
}

}  // namespace command
```

---

## Task 7: Refactor ZSet Commands

**Files:** Modify `src/command/zset_cmd.h`, `src/command/zset_cmd.cpp`

- [ ] **Step 1: Rewrite zset_cmd.h** (same pattern, ZAddCmd + ZScoreCmd)

- [ ] **Step 2: Rewrite zset_cmd.cpp**

Note: keep the existing `FormatScore` helper to match Redis-compatible output (e.g., `"3.14"` not `"3.140000"`). Existing integration tests assume this format.

```cpp
#include "command/zset_cmd.h"

#include <iomanip>
#include <locale>
#include <sstream>

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

  engine.Update(key, [&](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    cache::ZSetValue zset;
    std::uint64_t deadline_us = 0;
    if (existing) {
      if (existing->Type() != cache::RedisObjectType::kZSet) {
        wrong_type = true;
        return std::nullopt;
      }
      zset = *existing->ZSet();
      deadline_us = existing->DeadlineUs();
      created = zset.Find(cache::PackedString(member)) == nullptr;
    } else {
      created = true;
    }
    cache::ZSetValue next = zset.Set(cache::PackedString(member), score);
    cache::RedisObject obj = cache::RedisObject::MakeZSet(std::move(next));
    return deadline_us ? obj.WithDeadline(deadline_us) : obj;
  }, now_us);

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
  const double* found = obj->ZSet()->Find(cache::PackedString(args[2]));
  if (!found) return protocol::Response::NullBulk();
  return protocol::Response::BulkString(FormatScore(*found));
}

}  // namespace command
```

---

## Task 8: Refactor Key Commands

**Files:** Modify `src/command/key_cmd.h`, `src/command/key_cmd.cpp`

- [ ] **Step 1: Rewrite key_cmd.h** (DelCmd, ExpireCmd, TtlCmd, no member variables)

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

- [ ] **Step 1: Verify compilation**

Run: `make build`

- [ ] **Step 2: Update tests/cache_command_test.cpp**

Replace direct command construction with CommandDispatcher::Execute calls:

```cpp
// Old pattern:
//   command::SetCmd set_cmd("k", "v");
//   auto response = set_cmd.ExecCmd(engine, now_us);
//
// New pattern:
//   command::CommandDispatcher dispatcher;
//   auto response = dispatcher.Execute({"SET", "k", "v"}, engine, now_us);
```

- [ ] **Step 3: Run all tests**

Run: `make test`

- [ ] **Step 4: Commit all changes**

```bash
git add src/command/ src/cache/ tests/cache_command_test.cpp
git commit -m "refactor: stateless commands with registry-based dispatch

- RedisCmd: add CheckArity, ExecCmd accepts args vector
- CommandDispatcher: O(1) map lookup, static command instances
- All commands: stateless, use CacheEngine generic Get/Set/Update
- CacheEngine: add generic Get/Set/Update (returns copies, not pointers)
- HashSlot: add generic Get/Set/Update with proper locking"
```

---

## Task 10: Remove Old CacheEngine Methods

**Files:** Modify `src/cache/cache_engine.h`, `src/cache/cache_engine.cpp`

- [ ] **Step 1: Remove old method declarations from cache_engine.h**

Delete: GetString, SetString, HSet, HGet, SAdd, SIsMember, ZAdd, ZScore

- [ ] **Step 2: Remove old method implementations from cache_engine.cpp**

- [ ] **Step 3: Run tests**

Run: `make test`

- [ ] **Step 4: Commit**

```bash
git add src/cache/cache_engine.h src/cache/cache_engine.cpp
git commit -m "refactor: remove old command-specific CacheEngine methods"
```

---

## Task 11: Performance Validation

- [ ] **Step 1:** Run `make bench`
- [ ] **Step 2:** Verify performance is equal or better than baseline

---

## Completion Checklist

- [ ] All 11 commands refactored to stateless
- [ ] All tests passing
- [ ] No old CacheEngine command-specific methods remain
- [ ] Performance validated
- [ ] Code compiles without warnings
