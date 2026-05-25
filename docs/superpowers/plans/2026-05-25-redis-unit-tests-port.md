# Redis Unit Tests Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Redis 6.2 read-only query command support, then port runnable Redis 6.2 `tests/unit` cases for the supported command set without changing `src` during the port phase.

**Architecture:** Phase 1 adds `EXISTS`, `TYPE`, `PTTL`, and `SCAN` using the existing `RedisCmd` pattern, with keyspace metadata and slot traversal exposed as read-only cache APIs. Phase 2 adds a TCL RESP harness, copies Redis 6.2 unit tests, filters out unsupported command cases, and runs retained official test blocks against `cache_server`.

**Tech Stack:** C++17, CMake globbed sources/tests, custom `CACHE_TEST` harness, Tcl `tclsh`, shell scripts, Redis RESP2.

---

## File Map

- Create `src/common/glob_match.h`: public byte-oriented Redis glob matching API.
- Create `src/common/glob_match.cpp`: matcher implementation for `*`, `?`, bracket classes, negation, and backslash escapes.
- Modify `src/cache/hash_slot.h`: add read-only live-object traversal.
- Modify `src/cache/hash_slot.cpp`: implement traversal by snapshotting the immutable object map and skipping expired objects.
- Modify `src/cache/cache_engine.h`: expose slot-level live-object traversal.
- Modify `src/cache/cache_engine.cpp`: forward traversal to `HashSlot`.
- Modify `src/command/key_cmd.h`: declare `ExistsCmd`, `TypeCmd`, and `PTtlCmd`.
- Modify `src/command/key_cmd.cpp`: implement the three keyspace query commands.
- Create `src/command/scan_cmd.h`: declare `ScanCmd`.
- Create `src/command/scan_cmd.cpp`: parse `SCAN` options, perform slot-cursor scan, and build Redis response.
- Modify `src/command/command_dispatcher.cpp`: include `scan_cmd.h`, instantiate/register four read-only commands.
- Create `tests/glob_match_test.cpp`: focused matcher tests.
- Create `tests/key_query_command_test.cpp`: focused `EXISTS`, `TYPE`, and `PTTL` dispatcher tests.
- Create `tests/scan_command_test.cpp`: focused `SCAN` dispatcher tests.
- Create `tests/redis/harness/resp_client.tcl`: minimal RESP2 client for Tcl tests.
- Create `tests/redis/harness/cache_server_unit_runner.tcl`: lightweight Redis test helper subset and file runner.
- Create `tests/redis/run_unit_tests.sh`: starts `cache_server`, runs Tcl tests, and cleans up.
- Create `tests/redis/tools/filter_unit_tests.py`: filters copied Redis test files and writes `manifest.json`.
- Modify `CMakeLists.txt`: add `redis_unit_tests` CTest entry.
- Create `tests/redis/unit/`: filtered Redis 6.2 unit test tree.

## Execution Rule

Phase 1 tasks may edit `src`. Phase 2 tasks must not edit any `src` path. If a retained Redis unit test fails in Phase 2 because server behavior is incompatible, stop and report the failing official test rather than changing `src`.

---

### Task 1: Common Redis Glob Matcher

**Files:**
- Create: `src/common/glob_match.h`
- Create: `src/common/glob_match.cpp`
- Create: `tests/glob_match_test.cpp`

- [ ] **Step 1: Write matcher tests**

Create `tests/glob_match_test.cpp`:

```cpp
#include "test_harness.h"

#include <string>
#include <string_view>

#include "common/glob_match.h"

CACHE_TEST(GlobMatchSupportsRedisWildcards) {
  test::Require(common::GlobMatch("*", "abc"), "star matches all text");
  test::Require(common::GlobMatch("a*c", "abc"), "star matches middle text");
  test::Require(common::GlobMatch("a?c", "abc"), "question matches one byte");
  test::Require(!common::GlobMatch("a?c", "abbc"),
                "question does not match two bytes");
}

CACHE_TEST(GlobMatchSupportsCharacterClasses) {
  test::Require(common::GlobMatch("h[ae]llo", "hello"),
                "class matches first alternative");
  test::Require(common::GlobMatch("h[ae]llo", "hallo"),
                "class matches second alternative");
  test::Require(!common::GlobMatch("h[ae]llo", "hollo"),
                "class rejects missing alternative");
  test::Require(common::GlobMatch("item[0-9]", "item7"),
                "class range matches digit");
  test::Require(!common::GlobMatch("item[!0-9]", "item7"),
                "negated class rejects digit");
  test::Require(common::GlobMatch("item[!0-9]", "itemx"),
                "negated class accepts non-digit");
}

CACHE_TEST(GlobMatchSupportsEscapesAndBinaryStrings) {
  test::Require(common::GlobMatch(R"(a\*b)", "a*b"),
                "escaped star is literal");
  test::Require(common::GlobMatch(R"(a\?b)", "a?b"),
                "escaped question mark is literal");
  const std::string value(std::string("a", 1) + '\0' + "b");
  test::Require(common::GlobMatch(std::string_view("a?b", 3), value),
                "matcher is binary-safe for question mark");
  test::Require(!common::GlobMatch(std::string_view("a\\0b", 4), value),
                "backslash zero does not invent a NUL byte");
}
```

- [ ] **Step 2: Run tests to verify compile failure**

Run:

```bash
cmake --build build --target cache_tests
```

Expected: build fails because `common/glob_match.h` does not exist.

- [ ] **Step 3: Add public matcher header**

Create `src/common/glob_match.h`:

```cpp
#pragma once

#include <string_view>

namespace common {

bool GlobMatch(std::string_view pattern, std::string_view value);

template <typename Emit>
void EmitIfGlobMatches(std::string_view pattern, std::string_view value,
                       Emit&& emit) {
  if (GlobMatch(pattern, value)) {
    emit(value);
  }
}

}  // namespace common
```

- [ ] **Step 4: Add matcher implementation**

Create `src/common/glob_match.cpp`:

```cpp
#include "common/glob_match.h"

#include <cstddef>

namespace common {
namespace {

bool MatchRange(std::string_view pattern, std::size_t* pattern_index,
                unsigned char value) {
  std::size_t index = *pattern_index;
  bool negate = false;
  if (index < pattern.size() && (pattern[index] == '!' || pattern[index] == '^')) {
    negate = true;
    ++index;
  }

  bool matched = false;
  bool closed = false;
  while (index < pattern.size()) {
    unsigned char first = static_cast<unsigned char>(pattern[index]);
    if (first == ']' && index != *pattern_index) {
      closed = true;
      ++index;
      break;
    }
    if (first == '\\' && index + 1 < pattern.size()) {
      ++index;
      first = static_cast<unsigned char>(pattern[index]);
    }

    if (index + 2 < pattern.size() && pattern[index + 1] == '-' &&
        pattern[index + 2] != ']') {
      unsigned char last = static_cast<unsigned char>(pattern[index + 2]);
      if (last == '\\' && index + 3 < pattern.size()) {
        last = static_cast<unsigned char>(pattern[index + 3]);
        index += 4;
      } else {
        index += 3;
      }
      if (first <= value && value <= last) {
        matched = true;
      }
      continue;
    }

    if (first == value) {
      matched = true;
    }
    ++index;
  }

  if (!closed) {
    return false;
  }
  *pattern_index = index;
  return negate ? !matched : matched;
}

bool MatchAt(std::string_view pattern, std::size_t pattern_index,
             std::string_view value, std::size_t value_index) {
  while (pattern_index < pattern.size()) {
    const char token = pattern[pattern_index];
    if (token == '*') {
      while (pattern_index + 1 < pattern.size() &&
             pattern[pattern_index + 1] == '*') {
        ++pattern_index;
      }
      if (pattern_index + 1 == pattern.size()) {
        return true;
      }
      for (std::size_t next = value_index; next <= value.size(); ++next) {
        if (MatchAt(pattern, pattern_index + 1, value, next)) {
          return true;
        }
      }
      return false;
    }

    if (value_index >= value.size()) {
      return false;
    }

    if (token == '?') {
      ++pattern_index;
      ++value_index;
      continue;
    }

    if (token == '[') {
      std::size_t range_index = pattern_index + 1;
      if (!MatchRange(pattern, &range_index,
                      static_cast<unsigned char>(value[value_index]))) {
        return false;
      }
      pattern_index = range_index;
      ++value_index;
      continue;
    }

    char literal = token;
    if (literal == '\\' && pattern_index + 1 < pattern.size()) {
      ++pattern_index;
      literal = pattern[pattern_index];
    }
    if (literal != value[value_index]) {
      return false;
    }
    ++pattern_index;
    ++value_index;
  }
  return value_index == value.size();
}

}  // namespace

bool GlobMatch(std::string_view pattern, std::string_view value) {
  return MatchAt(pattern, 0, value, 0);
}

}  // namespace common
```

- [ ] **Step 5: Run matcher tests**

Run:

```bash
cmake --build build --target cache_tests
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: `cache_tests` passes, including the new glob matcher tests.

- [ ] **Step 6: Commit matcher**

```bash
git add src/common/glob_match.h src/common/glob_match.cpp tests/glob_match_test.cpp
git commit -m "feat: add Redis glob matcher"
```

---

### Task 2: Read-Only Cache Traversal

**Files:**
- Modify: `src/cache/hash_slot.h`
- Modify: `src/cache/hash_slot.cpp`
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`

- [ ] **Step 1: Add traversal declarations**

In `src/cache/hash_slot.h`, add `#include <functional>` if not already present and add this public method next to `Get`:

```cpp
  void ForEachLiveObject(
      std::uint64_t now_us,
      const std::function<void(const PackedString&, const RedisObject&)>&
          visitor) const;
```

In `src/cache/cache_engine.h`, add this public method next to `Get`:

```cpp
  void ForEachLiveObjectInSlot(
      std::size_t slot_id, std::uint64_t now_us,
      const std::function<void(const PackedString&, const RedisObject&)>&
          visitor) const;
```

- [ ] **Step 2: Implement slot traversal**

In `src/cache/hash_slot.cpp`, add this method after `HashSlot::Get`:

```cpp
void HashSlot::ForEachLiveObject(
    std::uint64_t now_us,
    const std::function<void(const PackedString&, const RedisObject&)>& visitor)
    const {
  ObjectMap snapshot;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    snapshot = redis_obj_map_;
  }

  snapshot.ForEach([&](const PackedString& key, const RedisObject& object) {
    if (!object.IsExpired(now_us)) {
      visitor(key, object);
    }
  });
}
```

- [ ] **Step 3: Implement engine forwarding**

In `src/cache/cache_engine.cpp`, add this method after `CacheEngine::Get`:

```cpp
void CacheEngine::ForEachLiveObjectInSlot(
    std::size_t slot_id, std::uint64_t now_us,
    const std::function<void(const PackedString&, const RedisObject&)>& visitor)
    const {
  SlotById(slot_id).ForEachLiveObject(now_us, visitor);
}
```

- [ ] **Step 4: Build**

Run:

```bash
cmake --build build --target cache_tests
```

Expected: build passes. There are no direct behavior tests yet; `SCAN` tests in Task 4 will exercise this path.

- [ ] **Step 5: Commit traversal**

```bash
git add src/cache/hash_slot.h src/cache/hash_slot.cpp src/cache/cache_engine.h src/cache/cache_engine.cpp
git commit -m "feat: add read-only slot traversal"
```

---

### Task 3: EXISTS, TYPE, And PTTL

**Files:**
- Modify: `src/command/key_cmd.h`
- Modify: `src/command/key_cmd.cpp`
- Modify: `src/command/command_dispatcher.cpp`
- Create: `tests/key_query_command_test.cpp`

- [ ] **Step 1: Write focused key query tests**

Create `tests/key_query_command_test.cpp`:

```cpp
#include "test_harness.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/response.h"

namespace {

protocol::Response Exec(command::CommandDispatcher& dispatcher,
                        cache::CacheEngine& engine, std::uint64_t now_us,
                        std::initializer_list<std::string_view> args) {
  std::vector<std::string> owned;
  owned.reserve(args.size());
  for (std::string_view arg : args) {
    owned.emplace_back(arg);
  }
  return dispatcher.Execute(owned, engine, now_us);
}

void RequireType(const protocol::Response& response,
                 protocol::ResponseType expected, std::string_view message) {
  test::Require(response.type == expected, message);
}

void RequireInteger(const protocol::Response& response, std::int64_t expected,
                    std::string_view message) {
  RequireType(response, protocol::ResponseType::kInteger, message);
  test::Require(response.integer == expected, message);
}

void RequireSimpleString(const protocol::Response& response,
                         std::string_view expected,
                         std::string_view message) {
  RequireType(response, protocol::ResponseType::kSimpleString, message);
  test::RequireEqual(response.text, expected, message);
}

}  // namespace

CACHE_TEST(ExistsCountsLiveKeysAndDuplicates) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "s", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  RequireInteger(Exec(dispatcher, engine, now_us, {"EXISTS", "s"}), 1,
                 "EXISTS returns 1 for live key");
  RequireInteger(Exec(dispatcher, engine, now_us, {"EXISTS", "s", "s", "h"}),
                 3, "EXISTS counts duplicate live keys");
  RequireInteger(
      Exec(dispatcher, engine, now_us + 2'000, {"EXISTS", "expired", "none"}),
      0, "EXISTS ignores expired and missing keys");
}

CACHE_TEST(TypeReportsStoredObjectType) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 2'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "str", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "hash", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SADD", "set", "m"});
  (void)Exec(dispatcher, engine, now_us, {"ZADD", "zset", "1", "m"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "str"}),
                      "string", "TYPE reports string");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "hash"}),
                      "hash", "TYPE reports hash");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "set"}), "set",
                      "TYPE reports set");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "zset"}),
                      "zset", "TYPE reports zset");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "none"}),
                      "none", "TYPE reports missing key");
  RequireSimpleString(
      Exec(dispatcher, engine, now_us + 2'000, {"TYPE", "expired"}), "none",
      "TYPE reports expired key as none");
}

CACHE_TEST(PTtlReportsMillisecondTtl) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 3'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "plain", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "volatile", "v", "PX", "1500"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  RequireInteger(Exec(dispatcher, engine, now_us, {"PTTL", "missing"}), -2,
                 "PTTL missing key returns -2");
  RequireInteger(Exec(dispatcher, engine, now_us, {"PTTL", "plain"}), -1,
                 "PTTL persistent key returns -1");
  const auto pttl = Exec(dispatcher, engine, now_us + 400'000,
                         {"PTTL", "volatile"});
  RequireType(pttl, protocol::ResponseType::kInteger,
              "PTTL volatile key returns integer");
  test::Require(pttl.integer >= 1000 && pttl.integer <= 1100,
                "PTTL returns remaining milliseconds");
  RequireInteger(Exec(dispatcher, engine, now_us + 2'000, {"PTTL", "expired"}),
                 -2, "PTTL expired key returns -2");
}
```

- [ ] **Step 2: Run tests to verify command failure**

Run:

```bash
cmake --build build --target cache_tests
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: build passes, then tests fail because `EXISTS`, `TYPE`, and `PTTL` return `ERR unknown command`.

- [ ] **Step 3: Declare command classes**

In `src/command/key_cmd.h`, add these classes before `DelCmd`:

```cpp
class ExistsCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class TypeCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class PTtlCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};
```

- [ ] **Step 4: Implement command helpers**

In `src/command/key_cmd.cpp`, add this helper in the anonymous namespace:

```cpp
std::string TypeName(cache::RedisObjectType type) {
  switch (type) {
    case cache::RedisObjectType::kString:
      return "string";
    case cache::RedisObjectType::kHash:
      return "hash";
    case cache::RedisObjectType::kSet:
      return "set";
    case cache::RedisObjectType::kZSet:
      return "zset";
  }
  return "none";
}
```

- [ ] **Step 5: Implement EXISTS, TYPE, and PTTL**

In `src/command/key_cmd.cpp`, add these methods before `DelCmd::CheckArity`:

```cpp
std::optional<std::string> ExistsCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 2) {
    return "ERR wrong number of arguments for 'exists' command";
  }
  return std::nullopt;
}

protocol::Response ExistsCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t count = 0;
  for (std::size_t index = 1; index < args.size(); ++index) {
    if (engine.Get(args[index], now_us).has_value()) {
      ++count;
    }
  }
  return protocol::Response::Integer(count);
}

std::optional<std::string> TypeCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'type' command";
  }
  return std::nullopt;
}

protocol::Response TypeCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto object = engine.Get(args[1], now_us);
  if (!object.has_value()) {
    return protocol::Response::SimpleString("none");
  }
  return protocol::Response::SimpleString(TypeName(object->Type()));
}

std::optional<std::string> PTtlCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'pttl' command";
  }
  return std::nullopt;
}

protocol::Response PTtlCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto object = engine.Get(args[1], now_us);
  if (!object.has_value()) {
    return protocol::Response::Integer(-2);
  }
  if (object->DeadlineUs() == 0) {
    return protocol::Response::Integer(-1);
  }
  return protocol::Response::Integer(
      static_cast<std::int64_t>((object->DeadlineUs() - now_us) / 1000ULL));
}
```

- [ ] **Step 6: Register commands**

In `src/command/command_dispatcher.cpp`, add static instances near key commands:

```cpp
  static ExistsCmd exists_cmd;
  static TypeCmd type_cmd;
  static PTtlCmd pttl_cmd;
```

Add registry entries near `DEL`, `EXPIRE`, and `TTL`:

```cpp
  commands_["EXISTS"] = {&exists_cmd, false};
  commands_["TYPE"] = {&type_cmd, false};
  commands_["PTTL"] = {&pttl_cmd, false};
```

- [ ] **Step 7: Run key query tests**

Run:

```bash
cmake --build build --target cache_tests
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: all `cache_tests` pass.

- [ ] **Step 8: Commit key query commands**

```bash
git add src/command/key_cmd.h src/command/key_cmd.cpp src/command/command_dispatcher.cpp tests/key_query_command_test.cpp
git commit -m "feat: add Redis key query commands"
```

---

### Task 4: SCAN Command

**Files:**
- Create: `src/command/scan_cmd.h`
- Create: `src/command/scan_cmd.cpp`
- Modify: `src/command/command_dispatcher.cpp`
- Create: `tests/scan_command_test.cpp`

- [ ] **Step 1: Write SCAN tests**

Create `tests/scan_command_test.cpp`:

```cpp
#include "test_harness.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/response.h"

namespace {

protocol::Response Exec(command::CommandDispatcher& dispatcher,
                        cache::CacheEngine& engine, std::uint64_t now_us,
                        std::initializer_list<std::string_view> args) {
  std::vector<std::string> owned;
  owned.reserve(args.size());
  for (std::string_view arg : args) {
    owned.emplace_back(arg);
  }
  return dispatcher.Execute(owned, engine, now_us);
}

void RequireArray(const protocol::Response& response,
                  std::string_view message) {
  test::Require(response.type == protocol::ResponseType::kArray, message);
}

std::vector<std::string> ScanKeys(const protocol::Response& response) {
  RequireArray(response, "SCAN returns array");
  test::Require(response.elements.size() == 2, "SCAN returns cursor and keys");
  test::Require(response.elements[0].type == protocol::ResponseType::kBulkString,
                "SCAN cursor is bulk string");
  RequireArray(response.elements[1], "SCAN keys are array");
  std::vector<std::string> keys;
  for (const protocol::Response& key : response.elements[1].elements) {
    test::Require(key.type == protocol::ResponseType::kBulkString,
                  "SCAN key is bulk string");
    keys.push_back(key.text);
  }
  return keys;
}

std::string ScanCursor(const protocol::Response& response) {
  RequireArray(response, "SCAN returns array");
  test::Require(response.elements.size() == 2, "SCAN returns two elements");
  test::Require(response.elements[0].type == protocol::ResponseType::kBulkString,
                "SCAN cursor is bulk string");
  return response.elements[0].text;
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

CACHE_TEST(ScanReturnsAllKeysAcrossStableIteration) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "scan:string", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "scan:hash", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SADD", "scan:set", "m"});
  (void)Exec(dispatcher, engine, now_us, {"ZADD", "scan:zset", "1", "m"});

  std::set<std::string> seen;
  std::string cursor = "0";
  for (int iteration = 0; iteration < 200000; ++iteration) {
    std::vector<std::string> args = {"SCAN", cursor, "COUNT", "1"};
    auto response = dispatcher.Execute(args, engine, now_us);
    for (const std::string& key : ScanKeys(response)) {
      seen.insert(key);
    }
    cursor = ScanCursor(response);
    if (cursor == "0") {
      break;
    }
  }

  test::Require(seen.count("scan:string") == 1, "SCAN finds string key");
  test::Require(seen.count("scan:hash") == 1, "SCAN finds hash key");
  test::Require(seen.count("scan:set") == 1, "SCAN finds set key");
  test::Require(seen.count("scan:zset") == 1, "SCAN finds zset key");
}

CACHE_TEST(ScanSupportsMatchAndTypeFilters) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 2'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "user:1", "a"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "user:2", "b"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "user:hash", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "other:1", "c"});

  auto match = Exec(dispatcher, engine, now_us,
                    {"SCAN", "0", "MATCH", "user:*", "COUNT", "100000"});
  const auto match_keys = ScanKeys(match);
  test::Require(Contains(match_keys, "user:1"), "MATCH includes user:1");
  test::Require(Contains(match_keys, "user:2"), "MATCH includes user:2");
  test::Require(Contains(match_keys, "user:hash"), "MATCH includes user:hash");
  test::Require(!Contains(match_keys, "other:1"), "MATCH excludes other:1");

  auto type = Exec(dispatcher, engine, now_us,
                   {"SCAN", "0", "MATCH", "user:*", "TYPE", "string",
                    "COUNT", "100000"});
  const auto type_keys = ScanKeys(type);
  test::Require(Contains(type_keys, "user:1"), "TYPE string includes string");
  test::Require(Contains(type_keys, "user:2"), "TYPE string includes string 2");
  test::Require(!Contains(type_keys, "user:hash"),
                "TYPE string excludes hash");
}

CACHE_TEST(ScanSkipsExpiredKeysAndRejectsInvalidSyntax) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 3'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "live", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  auto response =
      Exec(dispatcher, engine, now_us + 2'000, {"SCAN", "0", "COUNT", "100000"});
  const auto keys = ScanKeys(response);
  test::Require(Contains(keys, "live"), "SCAN includes live key");
  test::Require(!Contains(keys, "expired"), "SCAN skips expired key");

  test::Require(Exec(dispatcher, engine, now_us, {"SCAN"})
                    .type == protocol::ResponseType::kError,
                "SCAN requires cursor");
  test::Require(Exec(dispatcher, engine, now_us, {"SCAN", "bad"})
                    .type == protocol::ResponseType::kError,
                "SCAN rejects non-integer cursor");
  test::Require(Exec(dispatcher, engine, now_us,
                     {"SCAN", "0", "COUNT", "bad"})
                    .type == protocol::ResponseType::kError,
                "SCAN rejects non-integer count");
  test::Require(Exec(dispatcher, engine, now_us,
                     {"SCAN", "0", "UNKNOWN", "x"})
                    .type == protocol::ResponseType::kError,
                "SCAN rejects unknown option");
}
```

- [ ] **Step 2: Run tests to verify unknown command failure**

Run:

```bash
cmake --build build --target cache_tests
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: tests fail because `SCAN` is not registered.

- [ ] **Step 3: Declare SCAN command**

Create `src/command/scan_cmd.h`:

```cpp
#pragma once

#include "command/redis_cmd.h"

namespace command {

class ScanCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
```

- [ ] **Step 4: Implement SCAN**

Create `src/command/scan_cmd.cpp`:

```cpp
#include "command/scan_cmd.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/glob_match.h"
#include "common/parse_utils.h"

namespace command {
namespace {

constexpr std::size_t kDefaultCount = 1000;

struct ScanOptions {
  std::size_t cursor = 0;
  std::size_t count = kDefaultCount;
  std::optional<std::string> match;
  std::optional<std::string> type;
};

std::string ObjectTypeName(cache::RedisObjectType type) {
  switch (type) {
    case cache::RedisObjectType::kString:
      return "string";
    case cache::RedisObjectType::kHash:
      return "hash";
    case cache::RedisObjectType::kSet:
      return "set";
    case cache::RedisObjectType::kZSet:
      return "zset";
  }
  return "none";
}

bool ParseNonNegativeSize(std::string_view text, std::size_t* out) {
  std::int64_t parsed = 0;
  if (!common::ParseInt64(text, &parsed) || parsed < 0) {
    return false;
  }
  *out = static_cast<std::size_t>(parsed);
  return true;
}

std::optional<std::string> ParseOptions(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    ScanOptions* options) {
  if (!ParseNonNegativeSize(args[1], &options->cursor) ||
      options->cursor >= engine.SlotCount()) {
    return "ERR invalid cursor";
  }

  for (std::size_t index = 2; index < args.size();) {
    const std::string option = common::ToUpperAscii(args[index]);
    if (option == "MATCH") {
      if (index + 1 >= args.size()) {
        return "ERR syntax error";
      }
      options->match = std::string(args[index + 1]);
      index += 2;
    } else if (option == "COUNT") {
      if (index + 1 >= args.size() ||
          !ParseNonNegativeSize(args[index + 1], &options->count)) {
        return "ERR value is not an integer or out of range";
      }
      index += 2;
    } else if (option == "TYPE") {
      if (index + 1 >= args.size()) {
        return "ERR syntax error";
      }
      options->type = common::ToUpperAscii(args[index + 1]);
      for (char& ch : *options->type) {
        if (ch >= 'A' && ch <= 'Z') {
          ch = static_cast<char>(ch - 'A' + 'a');
        }
      }
      index += 2;
    } else {
      return "ERR syntax error";
    }
  }
  return std::nullopt;
}

bool TypeMatches(const std::optional<std::string>& filter,
                 cache::RedisObjectType type) {
  if (!filter.has_value()) {
    return true;
  }
  return *filter == ObjectTypeName(type);
}

bool KeyMatches(const std::optional<std::string>& pattern,
                std::string_view key) {
  if (!pattern.has_value()) {
    return true;
  }
  return common::GlobMatch(*pattern, key);
}

}  // namespace

std::optional<std::string> ScanCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 2) {
    return "ERR wrong number of arguments for 'scan' command";
  }
  return std::nullopt;
}

protocol::Response ScanCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  ScanOptions options;
  if (auto error = ParseOptions(args, engine, &options)) {
    return protocol::Response::Error(*error);
  }

  std::vector<protocol::Response> keys;
  std::size_t scanned_keys = 0;
  std::size_t slot = options.cursor;
  while (slot < engine.SlotCount()) {
    engine.ForEachLiveObjectInSlot(
        slot, now_us,
        [&](const cache::PackedString& key, const cache::RedisObject& object) {
          ++scanned_keys;
          if (TypeMatches(options.type, object.Type()) &&
              KeyMatches(options.match, key.View())) {
            keys.push_back(protocol::Response::BulkString(key.View()));
          }
        });
    ++slot;
    if (scanned_keys >= options.count) {
      break;
    }
  }

  const std::size_t next_cursor = slot >= engine.SlotCount() ? 0 : slot;
  std::vector<protocol::Response> result;
  result.push_back(protocol::Response::BulkString(std::to_string(next_cursor)));
  result.push_back(protocol::Response::Array(std::move(keys)));
  return protocol::Response::Array(std::move(result));
}

}  // namespace command
```

- [ ] **Step 5: Register SCAN**

In `src/command/command_dispatcher.cpp`, add include:

```cpp
#include "command/scan_cmd.h"
```

Add static instance:

```cpp
  static ScanCmd scan_cmd;
```

Add registry entry:

```cpp
  commands_["SCAN"] = {&scan_cmd, false};
```

- [ ] **Step 6: Run SCAN tests**

Run:

```bash
cmake --build build --target cache_tests
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: all `cache_tests` pass.

- [ ] **Step 7: Commit SCAN**

```bash
git add src/command/scan_cmd.h src/command/scan_cmd.cpp src/command/command_dispatcher.cpp tests/scan_command_test.cpp
git commit -m "feat: add Redis SCAN command"
```

---

### Task 5: Phase 1 Verification

**Files:**
- No file edits unless verification exposes a defect in Phase 1 changes.

- [ ] **Step 1: Run full build**

Run:

```bash
cmake --build build --target cache_tests cache_server
```

Expected: both targets build successfully.

- [ ] **Step 2: Run full C++ test suite**

Run:

```bash
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: all `cache_tests` pass.

- [ ] **Step 3: Confirm no Phase 1 source debt**

Run:

```bash
git status --short src tests CMakeLists.txt
```

Expected: no unstaged Phase 1 edits remain. If there are staged or unstaged Phase 1 edits, inspect them and either commit them with a focused message or fix the previous task.

---

### Task 6: TCL RESP Harness

**Files:**
- Create: `tests/redis/harness/resp_client.tcl`
- Create: `tests/redis/harness/cache_server_unit_runner.tcl`
- Create: `tests/redis/run_unit_tests.sh`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add RESP client**

Create `tests/redis/harness/resp_client.tcl`:

```tcl
namespace eval resp {
    variable sock

    proc connect {host port} {
        variable sock
        set sock [socket $host $port]
        fconfigure $sock -translation binary -encoding binary -buffering none
    }

    proc close {} {
        variable sock
        if {[info exists sock]} {
            catch {::close $sock}
            unset sock
        }
    }

    proc write_command {args} {
        variable sock
        puts -nonewline $sock "*[llength $args]\r\n"
        foreach arg $args {
            set bytes [encoding convertto utf-8 $arg]
            puts -nonewline $sock "\$[string length $bytes]\r\n"
            puts -nonewline $sock $bytes
            puts -nonewline $sock "\r\n"
        }
        flush $sock
    }

    proc read_line {} {
        variable sock
        set line [gets $sock]
        if {[string index $line end] eq "\r"} {
            set line [string range $line 0 end-1]
        }
        return $line
    }

    proc read_exact {n} {
        variable sock
        set data ""
        while {[string length $data] < $n} {
            append data [read $sock [expr {$n - [string length $data]}]]
        }
        return $data
    }

    proc read_reply {} {
        variable sock
        set prefix [read $sock 1]
        switch -- $prefix {
            "+" {
                return [read_line]
            }
            "-" {
                error [read_line]
            }
            ":" {
                return [read_line]
            }
            "$" {
                set len [read_line]
                if {$len < 0} {
                    return {}
                }
                set data [read_exact $len]
                read_exact 2
                return $data
            }
            "*" {
                set count [read_line]
                if {$count < 0} {
                    return {}
                }
                set out {}
                for {set i 0} {$i < $count} {incr i} {
                    lappend out [read_reply]
                }
                return $out
            }
            default {
                error "invalid RESP prefix '$prefix'"
            }
        }
    }

    proc command {args} {
        write_command $args
        return [read_reply]
    }
}
```

- [ ] **Step 2: Add Tcl test runner**

Create `tests/redis/harness/cache_server_unit_runner.tcl`:

```tcl
set ::passed 0
set ::failed 0

source [file join [file dirname [info script]] resp_client.tcl]

proc r {args} {
    return [resp::command {*}$args]
}

proc assert_equal {expected actual} {
    if {$expected ne $actual} {
        error "assert_equal failed: expected '$expected' got '$actual'"
    }
}

proc assert_match {pattern actual} {
    if {![string match $pattern $actual]} {
        error "assert_match failed: pattern '$pattern' got '$actual'"
    }
}

proc assert_error {pattern body} {
    set err {}
    if {![catch {uplevel 1 $body} err]} {
        error "assert_error failed: command succeeded"
    }
    assert_match $pattern $err
}

proc assert_range {actual min max} {
    if {$actual < $min || $actual > $max} {
        error "assert_range failed: $actual not in $min..$max"
    }
}

proc assert {expr} {
    if {![uplevel 1 [list expr $expr]]} {
        error "assert failed: $expr"
    }
}

proc test {name body {expected __no_expected__}} {
    global passed failed
    set result {}
    set code [catch {uplevel 1 $body} result]
    if {$code != 0} {
        incr failed
        puts stderr "FAIL $name: $result"
        return
    }
    if {$expected ne "__no_expected__" && $result ne $expected} {
        incr failed
        puts stderr "FAIL $name: expected '$expected' got '$result'"
        return
    }
    incr passed
    puts "PASS $name"
}

proc start_server {args body} {
    uplevel 1 $body
}

proc tags {args body} {
    uplevel 1 $body
}

proc randomInt {max} {
    return [expr {int(rand() * $max)}]
}

proc randstring {min max {type alpha}} {
    set len [expr {$min + [randomInt [expr {$max - $min + 1}]]}]
    set chars abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
    set out ""
    for {set i 0} {$i < $len} {incr i} {
        append out [string index $chars [randomInt [string length $chars]]]
    }
    return $out
}

proc randomValue {} {
    return [randstring 1 16 alpha]
}

if {$argc < 3} {
    puts stderr "usage: cache_server_unit_runner.tcl host port file ?file...?"
    exit 2
}

set host [lindex $argv 0]
set port [lindex $argv 1]
resp::connect $host $port
foreach file [lrange $argv 2 end] {
    source $file
}
resp::close

puts "Redis unit tests: $::passed passed, $::failed failed"
if {$::failed != 0} {
    exit 1
}
```

- [ ] **Step 3: Add shell wrapper**

Create `tests/redis/run_unit_tests.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="${ROOT}/build/cache_server"
PORT="${CACHE_SERVER_REDIS_UNIT_PORT:-6399}"
HOST="127.0.0.1"

if [[ ! -x "${SERVER}" ]]; then
  echo "missing ${SERVER}; build cache_server first" >&2
  exit 2
fi

"${SERVER}" "${PORT}" 1 64 &
SERVER_PID=$!
cleanup() {
  kill "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

for attempt in $(seq 1 50); do
  if (exec 3<>"/dev/tcp/${HOST}/${PORT}") 2>/dev/null; then
    exec 3<&-
    exec 3>&-
    break
  fi
  sleep 0.1
done

mapfile -t TEST_FILES < <(find "${ROOT}/tests/redis/unit" -type f -name '*.tcl' | sort)
if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
  echo "no redis unit test files found" >&2
  exit 2
fi

exec tclsh "${ROOT}/tests/redis/harness/cache_server_unit_runner.tcl" \
  "${HOST}" "${PORT}" "${TEST_FILES[@]}"
```

- [ ] **Step 4: Make wrapper executable**

Run:

```bash
chmod +x tests/redis/run_unit_tests.sh
```

- [ ] **Step 5: Add CTest target**

In `CMakeLists.txt`, add after the existing `add_test(NAME cache_tests COMMAND cache_tests)`:

```cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/redis/run_unit_tests.sh")
  add_test(NAME redis_unit_tests
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/redis/run_unit_tests.sh")
endif()
```

- [ ] **Step 6: Build to validate CMake**

Run:

```bash
cmake --build build --target cache_server
ctest --test-dir build -N
```

Expected: `redis_unit_tests` appears in the CTest listing after CMake reconfiguration. If it does not appear, run `cmake -S . -B build` and repeat `ctest --test-dir build -N`.

- [ ] **Step 7: Commit harness**

```bash
git add CMakeLists.txt tests/redis/harness/resp_client.tcl tests/redis/harness/cache_server_unit_runner.tcl tests/redis/run_unit_tests.sh
git commit -m "test: add Redis unit test harness"
```

---

### Task 7: Import And Filter Redis 6.2 Unit Tests

**Files:**
- Create: `tests/redis/tools/filter_unit_tests.py`
- Create/modify: `tests/redis/unit/**`
- Create: `tests/redis/unit/manifest.json`

- [ ] **Step 1: Add filtering tool**

Create `tests/redis/tools/filter_unit_tests.py`:

```python
#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

SUPPORTED = {
    "set", "get", "mget", "setnx", "getset", "strlen", "append",
    "incr", "decr", "incrby", "decrby",
    "hset", "hget", "hdel", "hexists", "hlen", "hstrlen", "hmget",
    "hmset", "hgetall", "hkeys", "hvals", "hincrby",
    "sadd", "sismember", "srem", "scard", "smembers", "smismember",
    "spop", "srandmember",
    "zadd", "zscore", "zrem", "zcard", "zrank", "zrevrank",
    "zcount", "zincrby", "zrange",
    "del", "expire", "ttl", "exists", "type", "pttl", "scan",
}

DROP_WORDS = {
    "config", "debug", "object", "memory", "flushdb", "flushall", "dbsize",
    "keys", "randomkey", "time", "multi", "exec", "watch", "select",
    "bgsave", "save", "restore", "dump", "client", "command", "info",
    "script", "eval", "evalsha", "acl", "auth", "subscribe", "publish",
}

COMMAND_RE = re.compile(r"\br\s+([A-Za-z][A-Za-z0-9_-]*)")


def parse_braced(text, index):
    assert text[index] == "{"
    depth = 0
    escaped = False
    for pos in range(index, len(text)):
        ch = text[pos]
        if escaped:
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[index + 1:pos], pos + 1
    raise ValueError("unclosed brace")


def skip_space(text, index):
    while index < len(text) and text[index].isspace():
        index += 1
    return index


def parse_word(text, index):
    index = skip_space(text, index)
    if index >= len(text):
        return "", index
    if text[index] == "{":
        return parse_braced(text, index)
    if text[index] == '"':
        end = index + 1
        escaped = False
        while end < len(text):
            ch = text[end]
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                return text[index + 1:end], end + 1
            end += 1
        raise ValueError("unclosed quote")
    end = index
    while end < len(text) and not text[end].isspace():
        end += 1
    return text[index:end], end


def find_test_blocks(text):
    blocks = []
    pos = 0
    while True:
        match = re.search(r"(^|[\s;])test\s+", text[pos:])
        if not match:
            break
        start = pos + match.start() + len(match.group(1))
        index = start + len("test")
        try:
            title, index = parse_word(text, index)
            body_start = skip_space(text, index)
            if body_start >= len(text) or text[body_start] != "{":
                pos = start + 4
                continue
            body, body_end = parse_braced(text, body_start)
            end = body_end
            expected_start = skip_space(text, body_end)
            if expected_start < len(text) and text[expected_start] in '{"':
                _, end = parse_word(text, expected_start)
            blocks.append((start, end, title, body))
            pos = end
        except ValueError:
            pos = start + 4
    return blocks


def commands_in_body(body):
    return {match.group(1).lower() for match in COMMAND_RE.finditer(body)}


def should_keep(title, body):
    commands = commands_in_body(body)
    unsupported = sorted(cmd for cmd in commands if cmd not in SUPPORTED)
    lowered = f"{title}\n{body}".lower()
    blocked = sorted(word for word in DROP_WORDS if word in lowered)
    if unsupported:
        return False, f"unsupported command(s): {', '.join(unsupported)}"
    if blocked:
        return False, f"out-of-scope helper/capability: {', '.join(blocked)}"
    if "slow" in lowered or "stress" in lowered:
        return False, "slow or stress test"
    return True, "retained"


def filter_file(path):
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    blocks = find_test_blocks(text)
    if not blocks:
        return {"retained": [], "deleted": []}
    retained = []
    deleted = []
    output = []
    last = 0
    for start, end, title, body in blocks:
        output.append(text[last:start])
        keep, reason = should_keep(title, body)
        if keep:
            output.append(text[start:end])
            retained.append(title)
        else:
            output.append(f"# Deleted during cache_server port: {reason}; test: {title}\n")
            deleted.append({"title": title, "reason": reason})
        last = end
    output.append(text[last:])
    path.write_text("".join(output), encoding="utf-8", errors="surrogateescape")
    return {"retained": retained, "deleted": deleted}


def main():
    if len(sys.argv) != 2:
        print("usage: filter_unit_tests.py tests/redis/unit", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    manifest = {"redis_ref": "6.2", "files": {}}
    for path in sorted(root.rglob("*.tcl")):
        rel = str(path.relative_to(root))
        result = filter_file(path)
        if not result["retained"]:
            path.unlink()
            if not any(path.parent.iterdir()):
                try:
                    path.parent.rmdir()
                except OSError:
                    pass
        manifest["files"][rel] = result
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Import Redis 6.2 unit directory**

Run:

```bash
rm -rf /tmp/redis-6.2-src /tmp/redis-6.2.tar.gz
curl -L https://github.com/redis/redis/archive/refs/heads/6.2.tar.gz -o /tmp/redis-6.2.tar.gz
mkdir -p /tmp/redis-6.2-src
tar -xzf /tmp/redis-6.2.tar.gz -C /tmp/redis-6.2-src --strip-components=1
rm -rf tests/redis/unit
mkdir -p tests/redis
cp -R /tmp/redis-6.2-src/tests/unit tests/redis/unit
```

Expected: `tests/redis/unit` contains the copied Redis 6.2 unit tree.

- [ ] **Step 3: Filter unsupported test blocks**

Run:

```bash
python3 tests/redis/tools/filter_unit_tests.py tests/redis/unit
```

Expected: unsupported test blocks are replaced with deletion comments, files with no retained tests are removed, and `tests/redis/unit/manifest.json` is created.

- [ ] **Step 4: Inspect retained commands**

Run:

```bash
rg '\br\s+([A-Za-z][A-Za-z0-9_-]*)' tests/redis/unit
```

Expected: visible `r <command>` calls are limited to the supported command set. If unsupported commands remain, update `filter_unit_tests.py` to recognize that syntax and rerun Step 3 from a fresh copy.

- [ ] **Step 5: Commit imported tests**

```bash
git add tests/redis/tools/filter_unit_tests.py tests/redis/unit
git commit -m "test: import filtered Redis unit tests"
```

---

### Task 8: Redis Unit Test Verification

**Files:**
- May modify only `tests/redis/**`, `CMakeLists.txt`, or docs if verification exposes a test harness problem.
- Do not modify `src/**`.

- [ ] **Step 1: Build server**

Run:

```bash
cmake --build build --target cache_server
```

Expected: `build/cache_server` exists and is executable.

- [ ] **Step 2: Run Redis unit CTest**

Run:

```bash
ctest --test-dir build --output-on-failure -R redis_unit_tests
```

Expected: retained Redis unit tests pass. If the CTest is not listed, run:

```bash
cmake -S . -B build
ctest --test-dir build -N
```

Then rerun the Redis unit CTest.

- [ ] **Step 3: Handle harness-only failures**

If a failure is caused by Tcl helper behavior and not server behavior, edit only `tests/redis/harness/*.tcl` and rerun:

```bash
ctest --test-dir build --output-on-failure -R redis_unit_tests
```

Expected: the harness supports retained official tests without simulating Redis commands.

- [ ] **Step 4: Handle unsupported-command leftovers**

If a retained test still calls an unsupported command, edit only `tests/redis/tools/filter_unit_tests.py`, recopy Redis tests from `/tmp/redis-6.2-src/tests/unit`, rerun filtering, then rerun:

```bash
ctest --test-dir build --output-on-failure -R redis_unit_tests
```

Expected: the unsupported test block is deleted and recorded in `manifest.json`.

- [ ] **Step 5: Commit verification fixes**

If Steps 3 or 4 changed files, commit them:

```bash
git add tests/redis CMakeLists.txt
git commit -m "test: stabilize Redis unit test port"
```

---

### Task 9: Final Verification

**Files:**
- No planned edits.

- [ ] **Step 1: Run C++ tests**

Run:

```bash
cmake --build build --target cache_tests cache_server
ctest --test-dir build --output-on-failure -R cache_tests
```

Expected: all C++ tests pass.

- [ ] **Step 2: Run Redis unit tests**

Run:

```bash
ctest --test-dir build --output-on-failure -R redis_unit_tests
```

Expected: all retained Redis unit tests pass.

- [ ] **Step 3: Confirm Phase 2 did not modify src**

Run:

```bash
git diff --name-only HEAD~3..HEAD -- src
```

Expected: this may show Phase 1 source files in earlier commits, but no commit after the Redis unit-test import should contain `src` paths. If a Phase 2 commit touched `src`, stop and split or revert that Phase 2 source change before completion.

- [ ] **Step 4: Check working tree**

Run:

```bash
git status --short
```

Expected: only pre-existing unrelated dirty paths remain, such as build directories or third-party submodule dirt. There should be no unstaged files from this plan.
