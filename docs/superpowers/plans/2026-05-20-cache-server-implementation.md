# Cache Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a C++17 Redis RESP2-compatible cache server with slot-level immutable data, the approved Redis 6.2 command subset, single-slave asynchronous replication, and single-threaded expiration cleanup.

**Architecture:** The server is split into protocol, command, cache, replication, expiration, and network modules. Worker coroutines parse RESP commands and call a polymorphic command layer; `CacheEngine` owns `100003` encapsulated slots, each slot publishes immutable versions and ordered binlog records. Replication uses one master sync thread, one slave receiver, 2-4 slave apply workers, and periodic applied-seq ACKs.

**Tech Stack:** C++17, CMake, libco, cpp_util RESP helpers, cpp_util immutable containers, optional jemalloc, custom lightweight C++ test harness.

---

## Scope Check

The spec covers one cohesive cache server. The plan is phased so each phase produces compiling, testable software:

1. Build and test harness.
2. Protocol and command result primitives.
3. Immutable cache engine and command execution.
4. Expiration.
5. Replication state machines and apply queues.
6. Libco networking and process wiring.
7. Integration tests and benchmarks.

## File Structure

Create these files:

- `CMakeLists.txt`: root build for library, server binary, tests, and optional jemalloc.
- `src/common/hash.h`: stable FNV-1a hash and slot mapping helpers.
- `src/common/time.h`: monotonic microsecond clock wrapper.
- `src/cache_core_anchor.cpp`: temporary non-empty source file for the core
  library before feature sources are added.
- `src/protocol/response.h`: typed RESP response model.
- `src/protocol/resp_codec.h`, `src/protocol/resp_codec.cpp`: RESP parsing and packing wrapper around `cpp_util/redis`.
- `src/cache/redis_object.h`, `src/cache/redis_object.cpp`: immutable object variant, TTL handling, type checks.
- `src/cache/binlog.h`, `src/cache/binlog.cpp`: canonical write log record and backlog buffer.
- `src/cache/hash_slot.h`, `src/cache/hash_slot.cpp`: slot locks, map publication, snapshot capture, log cleanup.
- `src/cache/cache_engine.h`, `src/cache/cache_engine.cpp`: high-level cache API over `SlotTable`.
- `src/command/redis_cmd.h`: polymorphic `RedisCmd` interface and parsed command aliases.
- `src/command/command_dispatcher.h`, `src/command/command_dispatcher.cpp`: RESP args to command objects.
- `src/command/string_cmd.h`, `src/command/string_cmd.cpp`: `SET`, `GET`.
- `src/command/hash_cmd.h`, `src/command/hash_cmd.cpp`: `HSET`, `HGET`.
- `src/command/set_cmd.h`, `src/command/set_cmd.cpp`: `SADD`, `SISMEMBER`.
- `src/command/zset_cmd.h`, `src/command/zset_cmd.cpp`: `ZADD`, `ZSCORE`.
- `src/command/key_cmd.h`, `src/command/key_cmd.cpp`: `DEL`, `EXPIRE`, `TTL`.
- `src/expire/expire_sweeper.h`, `src/expire/expire_sweeper.cpp`: single-threaded cleanup loop.
- `src/repl/repl_frame.h`, `src/repl/repl_frame.cpp`: `CACHE.REPL` frame encoding and decoding.
- `src/repl/master_replicator.h`, `src/repl/master_replicator.cpp`: single-slave master sync state.
- `src/repl/slave_replicator.h`, `src/repl/slave_replicator.cpp`: receiver, apply workers, ACK aggregation.
- `src/net/server.h`, `src/net/server.cpp`: libco accept and connection workers.
- `src/main.cpp`: CLI config and process startup.
- `tests/test_harness.h`, `tests/test_harness.cpp`: tiny test registry and assertions.
- `tests/cache_command_test.cpp`: command semantics.
- `tests/hash_slot_test.cpp`: slot locking and binlog behavior.
- `tests/resp_codec_test.cpp`: RESP parsing and packing.
- `tests/expire_test.cpp`: expiration cleanup.
- `tests/repl_test.cpp`: replication frame, backlog, resume, apply worker behavior.
- `tests/integration_resp_test.cpp`: socket-level RESP smoke tests.
- `bench/run_single_worker_qps.sh`: basic pipeline benchmark driver.

Modify these files:

- `Makefile`: keep submodule targets and add thin wrappers for CMake build/test/bench.

Do not modify `thirdparty/*`.

### Task 1: Build And Test Harness

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/cache_core_anchor.cpp`
- Create: `tests/test_harness.h`
- Create: `tests/test_harness.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write the failing build/test smoke test**

Create `tests/test_harness.h` with this interface:

```cpp
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace test {

using TestFn = std::function<void()>;

struct TestCase {
  std::string name;
  TestFn fn;
};

void Register(std::string name, TestFn fn);
int RunAll();
void Require(bool condition, std::string_view message);
void RequireEqual(std::string_view actual, std::string_view expected,
                  std::string_view message);

}  // namespace test

#define CACHE_TEST(name)                                      \
  static void name();                                         \
  namespace {                                                 \
  struct name##_registrar {                                   \
    name##_registrar() { ::test::Register(#name, name); }     \
  } name##_registrar_instance;                                \
  }                                                           \
  static void name()
```

Create `tests/test_harness.cpp`:

```cpp
#include "test_harness.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace test {
namespace {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

}  // namespace

void Register(std::string name, TestFn fn) {
  Registry().push_back(TestCase{std::move(name), std::move(fn)});
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void RequireEqual(std::string_view actual, std::string_view expected,
                  std::string_view message) {
  if (actual != expected) {
    throw std::runtime_error(std::string(message) + ": expected [" +
                             std::string(expected) + "], got [" +
                             std::string(actual) + "]");
  }
}

int RunAll() {
  int failed = 0;
  for (const TestCase& test_case : Registry()) {
    try {
      test_case.fn();
      std::cout << "[PASS] " << test_case.name << "\n";
    } catch (const std::exception& ex) {
      ++failed;
      std::cerr << "[FAIL] " << test_case.name << ": " << ex.what() << "\n";
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace test

int main() { return test::RunAll(); }
```

- [ ] **Step 2: Add the root CMake skeleton**

Create `src/cache_core_anchor.cpp`:

```cpp
namespace cache_server {

void CacheCoreAnchor() {}

}  // namespace cache_server
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(cache_server LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CACHE_SERVER_USE_JEMALLOC "Link bundled jemalloc when available" OFF)

add_subdirectory(thirdparty/libco)

file(GLOB_RECURSE CACHE_CORE_SRCS CONFIGURE_DEPENDS src/*.cpp)
list(FILTER CACHE_CORE_SRCS EXCLUDE REGEX ".*/main\\.cpp$")
add_library(cache_core STATIC ${CACHE_CORE_SRCS})

target_include_directories(cache_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp_util/redis/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp_util/immutable_container/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libco
)

target_link_libraries(cache_core PUBLIC colib_static pthread dl)

file(GLOB CACHE_TEST_SRCS CONFIGURE_DEPENDS tests/*_test.cpp)
add_executable(cache_tests tests/test_harness.cpp ${CACHE_TEST_SRCS})
target_link_libraries(cache_tests PRIVATE cache_core)
target_include_directories(cache_tests PRIVATE tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
  add_executable(cache_server src/main.cpp)
  target_link_libraries(cache_server PRIVATE cache_core)
endif()

enable_testing()
add_test(NAME cache_tests COMMAND cache_tests)
```

- [ ] **Step 3: Update Makefile wrappers**

Modify the root `Makefile` so the build targets are:

```make
.PHONY: all init update clean help configure build test bench

all: build

init:
	@echo "Initializing and updating submodules..."
	git submodule update --init --recursive

update:
	@echo "Updating all submodules to latest..."
	git submodule update --remote --recursive

update-jemalloc:
	@echo "Updating jemalloc..."
	git submodule update --remote thirdparty/jemalloc

update-libco:
	@echo "Updating libco..."
	git submodule update --remote thirdparty/libco

update-cpp_util:
	@echo "Updating cpp_util..."
	git submodule update --remote thirdparty/cpp_util

configure:
	cmake -S . -B build

build: configure
	cmake --build build -j

test: build
	cd build && ctest --output-on-failure

bench: build
	./bench/run_single_worker_qps.sh

clean:
	@echo "Removing build directory..."
	rm -rf build

status:
	@echo "Submodule status:"
	git submodule status

help:
	@echo "Available targets:"
	@echo "  make init"
	@echo "  make update"
	@echo "  make build"
	@echo "  make test"
	@echo "  make bench"
	@echo "  make clean"
```

- [ ] **Step 4: Run the build and observe the expected failure**

Run: `cmake -S . -B build && cmake --build build -j`

Expected: configure and build succeed; `cache_tests` has no registered feature tests yet.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt Makefile src/cache_core_anchor.cpp tests/test_harness.h tests/test_harness.cpp
git commit -m "build: add cmake and test harness"
```

### Task 2: Protocol Response And RESP Codec

**Files:**
- Create: `src/protocol/response.h`
- Create: `src/protocol/resp_codec.h`
- Create: `src/protocol/resp_codec.cpp`
- Create: `tests/resp_codec_test.cpp`

- [ ] **Step 1: Write RESP codec tests**

Create `tests/resp_codec_test.cpp`:

```cpp
#include "test_harness.h"

#include "protocol/resp_codec.h"

using protocol::RespCodec;
using protocol::Response;

CACHE_TEST(RespCodecParsesPipelineCommands) {
  RespCodec codec;
  codec.AppendBytes("*2\r\n$3\r\nGET\r\n$1\r\na\r\n*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\nc\r\n");

  auto first = codec.NextCommand();
  test::Require(first.has_value(), "first command is available");
  test::RequireEqual(first->args[0], "GET", "first command name");
  test::RequireEqual(first->args[1], "a", "first key");

  auto second = codec.NextCommand();
  test::Require(second.has_value(), "second command is available");
  test::RequireEqual(second->args[0], "SET", "second command name");
  test::RequireEqual(second->args[2], "c", "second value");
}

CACHE_TEST(RespCodecPacksResponses) {
  std::string out;
  protocol::PackResponse(Response::SimpleString("OK"), &out);
  protocol::PackResponse(Response::Integer(2), &out);
  protocol::PackResponse(Response::BulkString("abc"), &out);
  protocol::PackResponse(Response::NullBulk(), &out);
  test::RequireEqual(out, "+OK\r\n:2\r\n$3\r\nabc\r\n$-1\r\n", "packed RESP");
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because `protocol/resp_codec.h` is missing.

- [ ] **Step 3: Implement protocol response and codec**

Create `src/protocol/response.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace protocol {

enum class ResponseType {
  kSimpleString,
  kError,
  kInteger,
  kBulkString,
  kNullBulkString,
  kArray,
};

struct Response {
  ResponseType type = ResponseType::kNullBulkString;
  std::string text;
  std::int64_t integer = 0;
  std::vector<Response> elements;

  static Response SimpleString(std::string value);
  static Response Error(std::string value);
  static Response Integer(std::int64_t value);
  static Response BulkString(std::string value);
  static Response BulkString(std::string_view value);
  static Response NullBulk();
  static Response Array(std::vector<Response> values);
};

}  // namespace protocol
```

Create `src/protocol/resp_codec.h`:

```cpp
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/response.h"
#include "redis/resp.h"

namespace protocol {

struct CommandArgs {
  std::vector<std::string> args;
};

class RespCodec {
 public:
  explicit RespCodec(std::size_t max_stream_bytes = 4 * 1024 * 1024,
                     std::size_t max_bulk_bytes = 1024 * 1024,
                     std::size_t max_array_elements = 128);

  bool AppendBytes(std::string_view bytes);
  std::optional<CommandArgs> NextCommand();
  bool HasProtocolError() const;
  const std::string& ProtocolError() const;
  std::size_t BufferedBytes() const;

 private:
  bool ConvertCommand(const redis::RespValue& value, CommandArgs* out);

  std::string stream_;
  std::array<redis::RespValue, 256> scratch_{};
  redis::RespLimits limits_;
  std::size_t max_stream_bytes_;
  bool protocol_error_ = false;
  std::string protocol_error_text_;
};

void PackResponse(const Response& response, std::string* out);

}  // namespace protocol
```

Create `src/protocol/resp_codec.cpp` with `Response` factory methods, `RespCodec::NextCommand()` using `redis::UnpackOne()`, and `PackResponse()` using `redis::PackSimpleString`, `PackError`, `PackInteger`, `PackBulkString`, `PackNullBulkString`, and recursive `PackArrayHeader`.

- [ ] **Step 4: Run codec tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: `RespCodecParsesPipelineCommands` and `RespCodecPacksResponses` pass. Other tests may not exist yet or may be empty.

- [ ] **Step 5: Commit**

```bash
git add src/protocol/response.h src/protocol/resp_codec.h src/protocol/resp_codec.cpp tests/resp_codec_test.cpp
git commit -m "protocol: add RESP codec wrapper"
```

### Task 3: Cache Object, Binlog, And HashSlot Core

**Files:**
- Create: `src/common/hash.h`
- Create: `src/common/time.h`
- Create: `src/cache/redis_object.h`
- Create: `src/cache/redis_object.cpp`
- Create: `src/cache/binlog.h`
- Create: `src/cache/binlog.cpp`
- Create: `src/cache/hash_slot.h`
- Create: `src/cache/hash_slot.cpp`
- Create: `tests/hash_slot_test.cpp`

- [ ] **Step 1: Write slot and binlog tests**

Create `tests/hash_slot_test.cpp`:

```cpp
#include "test_harness.h"

#include "cache/hash_slot.h"

CACHE_TEST(HashSlotPublishesWriteAndBinlogAtomically) {
  cache::HashSlot slot;
  const std::uint64_t now_us = 1000;

  cache::WriteResult result = slot.SetString("key", "value", now_us);
  test::Require(result.changed, "SET changes slot");
  test::Require(result.seq == 1, "first write seq is 1");

  auto read = slot.GetString("key", now_us);
  test::Require(read.status == cache::Status::kOk, "string key exists");
  test::RequireEqual(read.value, "value", "string value");

  auto logs = slot.CopyLogsAfter(0, 10);
  test::Require(logs.size() == 1, "one log exists");
  test::Require(logs[0].seq == 1, "log seq matches published seq");

  slot.AckLogsThrough(1);
  test::Require(slot.CopyLogsAfter(0, 10).empty(), "acked log is removed");
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because `cache/hash_slot.h` is missing.

- [ ] **Step 3: Implement common helpers and object model**

Create `src/common/hash.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace common {

static constexpr std::size_t kSlotCount = 100003;

inline std::uint64_t Fnva64(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::size_t SlotForKey(std::string_view key) {
  return static_cast<std::size_t>(Fnva64(key) % kSlotCount);
}

}  // namespace common
```

Create `src/common/time.h`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>

namespace common {

inline std::uint64_t NowMicros() {
  using clock = std::chrono::steady_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          clock::now().time_since_epoch())
          .count());
}

}  // namespace common
```

Create `src/cache/redis_object.h` with these shared result and object types:

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "immutable_container/imt_map.h"
#include "immutable_container/imt_set.h"
#include "immutable_container/packed_string.h"
#include "immutable_container/ref_count_policy.h"

namespace cache {

using immutable_container::AtomicRefCount;
using immutable_container::ImtMap;
using immutable_container::ImtSet;
using immutable_container::PackedString;

enum class Status { kOk, kNotFound, kWrongType, kInvalidArgument };

template <typename T>
struct ReadResult {
  Status status = Status::kNotFound;
  T value{};
};

struct WriteResult {
  Status status = Status::kOk;
  bool changed = false;
  bool created = false;
  std::uint64_t seq = 0;
};

using HashValue = ImtMap<PackedString, PackedString, std::less<PackedString>, AtomicRefCount>;
using SetValue = ImtSet<PackedString, std::less<PackedString>, AtomicRefCount>;
using ZSetValue = ImtMap<PackedString, double, std::less<PackedString>, AtomicRefCount>;

enum class RedisObjectType { kString, kHash, kSet, kZSet };

class RedisObject {
 public:
  static RedisObject MakeString(std::string_view value);
  static RedisObject MakeHash(HashValue value);
  static RedisObject MakeSet(SetValue value);
  static RedisObject MakeZSet(ZSetValue value);

  RedisObjectType Type() const;
  bool IsExpired(std::uint64_t now_us) const;
  std::uint64_t DeadlineUs() const;
  RedisObject WithDeadline(std::uint64_t deadline_us) const;
  RedisObject ClearDeadline() const;

  const PackedString* StringValue() const;
  const HashValue* Hash() const;
  const SetValue* Set() const;
  const ZSetValue* ZSet() const;

 private:
  RedisObjectType type_ = RedisObjectType::kString;
  std::uint64_t deadline_us_ = 0;
  std::variant<PackedString, HashValue, SetValue, ZSetValue> value_;
};

using ObjectMap = ImtMap<PackedString, RedisObject, std::less<PackedString>, AtomicRefCount>;

}  // namespace cache
```

- [ ] **Step 4: Implement binlog and hash slot methods**

Create `src/cache/binlog.h`:

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace cache {

enum class BinlogOp {
  kSet,
  kDel,
  kExpire,
  kHSet,
  kSAdd,
  kZAdd,
};

struct BinlogRecord {
  std::uint64_t seq = 0;
  BinlogOp op = BinlogOp::kSet;
  std::vector<std::string> args;
  std::uint64_t remaining_ttl_us = 0;
};

class BinlogBuffer {
 public:
  void Append(BinlogRecord record);
  std::vector<BinlogRecord> CopyAfter(std::uint64_t seq, std::size_t limit) const;
  void AckThrough(std::uint64_t seq);
  std::uint64_t MinSeq() const;
  std::uint64_t MaxSeq() const;
  bool Empty() const;

 private:
  std::deque<BinlogRecord> records_;
};

}  // namespace cache
```

Create `src/cache/hash_slot.h` with private locks and public methods used by the tests:

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace cache {

struct SlotSnapshot {
  ObjectMap map;
  std::uint64_t published_seq = 0;
};

class HashSlot {
 public:
  WriteResult SetString(std::string_view key, std::string_view value,
                        std::uint64_t now_us);
  ReadResult<std::string> GetString(std::string_view key,
                                    std::uint64_t now_us) const;
  WriteResult Del(std::string_view key, std::uint64_t now_us);
  SlotSnapshot Snapshot() const;
  std::vector<BinlogRecord> CopyLogsAfter(std::uint64_t seq,
                                          std::size_t limit) const;
  void AckLogsThrough(std::uint64_t seq);

 private:
  mutable std::mutex write_mutex_;
  mutable std::mutex value_mutex_;
  ObjectMap redis_obj_map_;
  BinlogBuffer binlog_buffer_;
  std::uint64_t slot_seq_ = 0;
  std::uint64_t published_seq_ = 0;
};

}  // namespace cache
```

Implement `HashSlot::SetString()` with the approved commit order: acquire `write_mutex_`, build new object and log, append log with `next_seq`, then publish map and `published_seq_` under `value_mutex_`.

- [ ] **Step 5: Run slot tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: `HashSlotPublishesWriteAndBinlogAtomically` passes.

- [ ] **Step 6: Commit**

```bash
git add src/common/hash.h src/common/time.h src/cache/redis_object.h src/cache/redis_object.cpp src/cache/binlog.h src/cache/binlog.cpp src/cache/hash_slot.h src/cache/hash_slot.cpp tests/hash_slot_test.cpp
git commit -m "cache: add immutable slot core"
```

### Task 4: CacheEngine And String/Key Commands

**Files:**
- Create: `src/cache/cache_engine.h`
- Create: `src/cache/cache_engine.cpp`
- Create: `src/command/redis_cmd.h`
- Create: `src/command/string_cmd.h`
- Create: `src/command/string_cmd.cpp`
- Create: `src/command/key_cmd.h`
- Create: `src/command/key_cmd.cpp`
- Create: `tests/cache_command_test.cpp`

- [ ] **Step 1: Write command tests for string and key commands**

Create `tests/cache_command_test.cpp`:

```cpp
#include "test_harness.h"

#include "cache/cache_engine.h"
#include "command/key_cmd.h"
#include "command/string_cmd.h"

CACHE_TEST(StringAndKeyCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;

  command::SetCmd set_cmd("k", "v");
  auto set_response = set_cmd.ExecCmd(engine, now_us);
  test::RequireEqual(set_response.text, "OK", "SET returns OK");

  command::GetCmd get_cmd("k");
  auto get_response = get_cmd.ExecCmd(engine, now_us);
  test::RequireEqual(get_response.text, "v", "GET returns value");

  command::TtlCmd ttl_no_expire("k");
  test::Require(ttl_no_expire.ExecCmd(engine, now_us).integer == -1,
                "TTL without expire returns -1");

  command::ExpireCmd expire("k", 2);
  test::Require(expire.ExecCmd(engine, now_us).integer == 1,
                "EXPIRE existing key returns 1");

  command::TtlCmd ttl("k");
  test::Require(ttl.ExecCmd(engine, now_us + 500'000).integer == 1,
                "TTL floors remaining seconds");

  command::DelCmd del("k");
  test::Require(del.ExecCmd(engine, now_us + 500'000).integer == 1,
                "DEL removes key");
}
```

- [ ] **Step 2: Run the command test and verify it fails**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because `cache/cache_engine.h` and command headers are missing.

- [ ] **Step 3: Implement `CacheEngine` public API**

Create `src/cache/cache_engine.h`:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cache/hash_slot.h"
#include "common/hash.h"

namespace cache {

class CacheEngine {
 public:
  ReadResult<std::string> GetString(std::string_view key,
                                    std::uint64_t now_us) const;
  WriteResult SetString(std::string_view key, std::string_view value,
                        std::uint64_t now_us);
  WriteResult Del(std::string_view key, std::uint64_t now_us);
  bool Expire(std::string_view key, std::int64_t seconds,
              std::uint64_t now_us);
  std::int64_t Ttl(std::string_view key, std::uint64_t now_us) const;

  HashSlot& SlotForKey(std::string_view key);
  const HashSlot& SlotForKey(std::string_view key) const;
  HashSlot& SlotById(std::size_t slot_id);
  const HashSlot& SlotById(std::size_t slot_id) const;
  std::size_t SlotCount() const;

 private:
  std::array<HashSlot, common::kSlotCount> slots_;
};

}  // namespace cache
```

Create `src/command/redis_cmd.h`:

```cpp
#pragma once

#include <cstdint>

#include "cache/cache_engine.h"
#include "protocol/response.h"

namespace command {

class RedisCmd {
 public:
  virtual ~RedisCmd() = default;
  virtual protocol::Response ExecCmd(cache::CacheEngine& engine,
                                     std::uint64_t now_us) const = 0;
};

}  // namespace command
```

- [ ] **Step 4: Implement `SET/GET/DEL/EXPIRE/TTL` command classes**

Create `src/command/string_cmd.h` declaring `SetCmd` and `GetCmd`, each deriving from `RedisCmd`.

Create `src/command/key_cmd.h` declaring `DelCmd`, `ExpireCmd`, and `TtlCmd`, each deriving from `RedisCmd`.

Implementation rules:

- `SetCmd::ExecCmd()` calls `engine.SetString()` and returns `Response::SimpleString("OK")`.
- `GetCmd::ExecCmd()` calls `engine.GetString()` and returns bulk string, null bulk, or WRONGTYPE.
- `DelCmd::ExecCmd()` returns integer `1` when `engine.Del().changed`, else `0`.
- `ExpireCmd::ExecCmd()` returns integer `1` when `engine.Expire()` succeeds, else `0`.
- `TtlCmd::ExecCmd()` returns `Response::Integer(engine.Ttl())`.

- [ ] **Step 5: Run command tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: string/key command tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/cache/cache_engine.h src/cache/cache_engine.cpp src/command/redis_cmd.h src/command/string_cmd.h src/command/string_cmd.cpp src/command/key_cmd.h src/command/key_cmd.cpp tests/cache_command_test.cpp
git commit -m "command: add string and key commands"
```

### Task 5: Hash, Set, And ZSet Commands

**Files:**
- Modify: `src/cache/hash_slot.h`
- Modify: `src/cache/hash_slot.cpp`
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`
- Create: `src/command/hash_cmd.h`
- Create: `src/command/hash_cmd.cpp`
- Create: `src/command/set_cmd.h`
- Create: `src/command/set_cmd.cpp`
- Create: `src/command/zset_cmd.h`
- Create: `src/command/zset_cmd.cpp`
- Modify: `tests/cache_command_test.cpp`

- [ ] **Step 1: Extend command tests**

Append tests named:

```cpp
CACHE_TEST(HashSetAndZSetCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 10'000;

  test::Require(command::HSetCmd("h", "f", "v").ExecCmd(engine, now_us).integer == 1,
                "HSET new field returns 1");
  test::Require(command::HSetCmd("h", "f", "v2").ExecCmd(engine, now_us).integer == 0,
                "HSET existing field returns 0");
  test::RequireEqual(command::HGetCmd("h", "f").ExecCmd(engine, now_us).text,
                     "v2", "HGET returns updated value");

  test::Require(command::SAddCmd("s", "m").ExecCmd(engine, now_us).integer == 1,
                "SADD new member returns 1");
  test::Require(command::SAddCmd("s", "m").ExecCmd(engine, now_us).integer == 0,
                "SADD existing member returns 0");
  test::Require(command::SIsMemberCmd("s", "m").ExecCmd(engine, now_us).integer == 1,
                "SISMEMBER returns 1");

  test::Require(command::ZAddCmd("z", 1.5, "m").ExecCmd(engine, now_us).integer == 1,
                "ZADD new member returns 1");
  test::Require(command::ZAddCmd("z", 2.5, "m").ExecCmd(engine, now_us).integer == 0,
                "ZADD existing member returns 0");
  test::RequireEqual(command::ZScoreCmd("z", "m").ExecCmd(engine, now_us).text,
                     "2.5", "ZSCORE returns score");
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because hash/set/zset command classes are missing.

- [ ] **Step 3: Add cache engine methods**

Add methods to `CacheEngine` and `HashSlot`:

```cpp
WriteResult HSet(std::string_view key, std::string_view field,
                 std::string_view value, std::uint64_t now_us);
ReadResult<std::string> HGet(std::string_view key, std::string_view field,
                             std::uint64_t now_us) const;
WriteResult SAdd(std::string_view key, std::string_view member,
                 std::uint64_t now_us);
ReadResult<bool> SIsMember(std::string_view key, std::string_view member,
                           std::uint64_t now_us) const;
WriteResult ZAdd(std::string_view key, double score, std::string_view member,
                 std::uint64_t now_us);
ReadResult<double> ZScore(std::string_view key, std::string_view member,
                          std::uint64_t now_us) const;
```

For `WriteResult`, `status == Status::kWrongType` becomes WRONGTYPE, and `created` controls Redis integer return values for `HSET`, `SADD`, and `ZADD`. For `ReadResult`, `Status::kNotFound` becomes null bulk for `HGET` and `ZSCORE`, integer `0` for `SISMEMBER`, and `Status::kWrongType` becomes WRONGTYPE.

- [ ] **Step 4: Implement command files by data structure**

Create each command file with classes deriving from `RedisCmd`:

- `hash_cmd.*`: `HSetCmd`, `HGetCmd`
- `set_cmd.*`: `SAddCmd`, `SIsMemberCmd`
- `zset_cmd.*`: `ZAddCmd`, `ZScoreCmd`

For zset score formatting, use:

```cpp
std::string FormatScore(double score) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << score;
  std::string text = out.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}
```

- [ ] **Step 5: Run command tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: hash, set, and zset command tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/cache/hash_slot.h src/cache/hash_slot.cpp src/cache/cache_engine.h src/cache/cache_engine.cpp src/command/hash_cmd.h src/command/hash_cmd.cpp src/command/set_cmd.h src/command/set_cmd.cpp src/command/zset_cmd.h src/command/zset_cmd.cpp tests/cache_command_test.cpp
git commit -m "command: add hash set and zset commands"
```

### Task 6: Command Dispatcher

**Files:**
- Create: `src/command/command_dispatcher.h`
- Create: `src/command/command_dispatcher.cpp`
- Modify: `tests/cache_command_test.cpp`

- [ ] **Step 1: Write dispatcher tests**

Append:

```cpp
CACHE_TEST(CommandDispatcherParsesAndExecutesRespArgs) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1000;

  auto set = dispatcher.Execute({"set", "a", "1"}, engine, now_us);
  test::RequireEqual(set.text, "OK", "dispatcher executes SET");

  auto get = dispatcher.Execute({"GET", "a"}, engine, now_us);
  test::RequireEqual(get.text, "1", "dispatcher executes GET");

  auto wrong_arity = dispatcher.Execute({"GET"}, engine, now_us);
  test::Require(wrong_arity.type == protocol::ResponseType::kError,
                "wrong arity returns error");

  auto unknown = dispatcher.Execute({"NOPE", "a"}, engine, now_us);
  test::Require(unknown.type == protocol::ResponseType::kError,
                "unknown command returns error");
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because `CommandDispatcher` is missing.

- [ ] **Step 3: Implement dispatcher**

Create `src/command/command_dispatcher.h`:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "command/redis_cmd.h"

namespace command {

class CommandDispatcher {
 public:
  protocol::Response Execute(const std::vector<std::string>& args,
                             cache::CacheEngine& engine,
                             std::uint64_t now_us) const;

 private:
  std::unique_ptr<RedisCmd> Build(const std::vector<std::string>& args) const;
};

}  // namespace command
```

Implementation details:

- Convert command name to uppercase with ASCII rules.
- Validate exact arity for all 11 supported command forms.
- Parse `EXPIRE seconds` as signed integer.
- Parse `ZADD score` as finite double.
- Return Redis-style errors for unknown commands, wrong arity, and invalid score.

- [ ] **Step 4: Run dispatcher tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: dispatcher tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/command/command_dispatcher.h src/command/command_dispatcher.cpp tests/cache_command_test.cpp
git commit -m "command: add dispatcher"
```

### Task 7: Single-Threaded Expiration Sweeper

**Files:**
- Create: `src/expire/expire_sweeper.h`
- Create: `src/expire/expire_sweeper.cpp`
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`
- Create: `tests/expire_test.cpp`

- [ ] **Step 1: Write expiration tests**

Create `tests/expire_test.cpp`:

```cpp
#include "test_harness.h"

#include "cache/cache_engine.h"
#include "expire/expire_sweeper.h"

CACHE_TEST(ExpireSweeperDeletesExpiredKeysViaWritePath) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;
  engine.SetString("k", "v", now_us);
  test::Require(engine.Expire("k", 1, now_us), "expire succeeds");

  expire::ExpireSweeper sweeper(&engine, 8);
  std::size_t deleted = sweeper.SweepOnce(now_us + 2'000'000);
  test::Require(deleted == 1, "one expired key deleted");
  test::Require(engine.GetString("k", now_us + 2'000'000).status ==
                    cache::Status::kNotFound,
                "expired key is gone");
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because `expire/expire_sweeper.h` is missing.

- [ ] **Step 3: Implement scan API and sweeper**

Add `CacheEngine::DeleteExpiredInSlot(std::size_t slot_id, std::size_t max_keys, std::uint64_t now_us)` and `CacheEngine::SlotCount()`.

Create `src/expire/expire_sweeper.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include "cache/cache_engine.h"

namespace expire {

class ExpireSweeper {
 public:
  ExpireSweeper(cache::CacheEngine* engine, std::size_t slots_per_tick);
  std::size_t SweepOnce(std::uint64_t now_us);

 private:
  cache::CacheEngine* engine_;
  std::size_t slots_per_tick_;
  std::size_t next_slot_ = 0;
};

}  // namespace expire
```

`SweepOnce()` scans `slots_per_tick_` slots, calls `DeleteExpiredInSlot()`, advances `next_slot_`, and returns the delete count. Use the same slot write path so deletes generate binlog records.

- [ ] **Step 4: Run expiration tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: expiration tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/expire/expire_sweeper.h src/expire/expire_sweeper.cpp src/cache/cache_engine.h src/cache/cache_engine.cpp tests/expire_test.cpp
git commit -m "expire: add single-threaded sweeper"
```

### Task 8: Replication Frames, Backlog Resume, And ACK Cleanup

**Files:**
- Create: `src/repl/repl_frame.h`
- Create: `src/repl/repl_frame.cpp`
- Create: `src/repl/master_replicator.h`
- Create: `src/repl/master_replicator.cpp`
- Create: `tests/repl_test.cpp`

- [ ] **Step 1: Write frame and backlog tests**

Create `tests/repl_test.cpp`:

```cpp
#include "test_harness.h"

#include "cache/cache_engine.h"
#include "repl/master_replicator.h"
#include "repl/repl_frame.h"

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
  engine.SetString("k", "v1", 100);
  engine.SetString("k", "v2", 200);

  repl::MasterReplicator repl(&engine);
  repl.OnAck(common::SlotForKey("k"), 2);

  auto records = engine.SlotForKey("k").CopyLogsAfter(0, 10);
  test::Require(records.empty(), "acked logs are cleaned");
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because replication headers are missing.

- [ ] **Step 3: Implement replication frame model**

Create `src/repl/repl_frame.h` with:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cache/binlog.h"

namespace repl {

enum class Subcmd { kHello, kSnap, kSnapCommit, kLog, kAck, kOnline };

struct Frame {
  Subcmd subcmd = Subcmd::kHello;
  std::string repl_id;
  std::size_t slot_id = 0;
  std::uint64_t chunk_id = 0;
  std::uint64_t base_seq = 0;
  std::uint64_t end_seq = 0;
  cache::BinlogRecord record;
  std::vector<std::pair<std::size_t, std::uint64_t>> acked_slots;

  static Frame Log(std::size_t slot_id, cache::BinlogRecord record);
  static Frame Ack(std::vector<std::pair<std::size_t, std::uint64_t>> slots);
};

std::string EncodeFrame(const Frame& frame);
std::optional<Frame> DecodeFrame(std::string_view wire);

}  // namespace repl
```

Encode frames as RESP arrays using `CACHE.REPL` plus subcommand-specific bulk strings. Decode using `redis::UnpackOne()`.

- [ ] **Step 4: Implement master ACK cleanup**

Create `src/repl/master_replicator.h` with `MasterReplicator(cache::CacheEngine*)`, `OnAck(slot_id, applied_seq)`, and `BuildResumePlan(last_applied_seq_by_slot)`.

`OnAck()` calls `engine->SlotById(slot_id).AckLogsThrough(applied_seq)`.

- [ ] **Step 5: Run replication frame tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: replication frame and ACK cleanup tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/repl/repl_frame.h src/repl/repl_frame.cpp src/repl/master_replicator.h src/repl/master_replicator.cpp tests/repl_test.cpp
git commit -m "repl: add frames and master ack cleanup"
```

### Task 9: Slave Apply Workers And Snapshot Commit

**Files:**
- Create: `src/repl/slave_replicator.h`
- Create: `src/repl/slave_replicator.cpp`
- Modify: `tests/repl_test.cpp`

- [ ] **Step 1: Write slave apply ordering tests**

Append:

```cpp
CACHE_TEST(SlaveApplyHoldsOutOfOrderLogsUntilGapFilled) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord seq2;
  seq2.seq = 2;
  seq2.op = cache::BinlogOp::kSet;
  seq2.args = {"SET", "k", "v2"};

  cache::BinlogRecord seq1;
  seq1.seq = 1;
  seq1.op = cache::BinlogOp::kSet;
  seq1.args = {"SET", "k", "v1"};

  const std::size_t slot = common::SlotForKey("k");
  slave.ApplyLogForTest(slot, seq2, 1000);
  test::Require(engine.GetString("k", 1000).status == cache::Status::kNotFound,
                "seq2 waits for seq1");

  slave.ApplyLogForTest(slot, seq1, 1000);
  auto read = engine.GetString("k", 1000);
  test::Require(read.status == cache::Status::kOk, "key exists after gap fill");
  test::RequireEqual(read.value, "v2", "pending seq2 applies after seq1");
  test::Require(slave.AppliedSeqForTest(slot) == 2, "applied seq advances");
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: compile fails because `SlaveReplicator` is missing.

- [ ] **Step 3: Implement slave apply state**

Create `src/repl/slave_replicator.h` exposing:

```cpp
class SlaveReplicator {
 public:
  SlaveReplicator(cache::CacheEngine* engine, std::size_t apply_workers);
  void EnqueueFrame(Frame frame);
  void ApplyLogForTest(std::size_t slot_id, const cache::BinlogRecord& record,
                       std::uint64_t now_us);
  std::uint64_t AppliedSeqForTest(std::size_t slot_id) const;

 private:
  struct SlotApplyState;
  cache::CacheEngine* engine_;
  std::size_t apply_workers_;
};
```

Implementation details:

- Store per-slot `applied_seq`.
- Store per-slot pending logs keyed by seq.
- Apply only `seq == applied_seq + 1`.
- After each apply, drain consecutive pending records.
- Route frames by `slot_id % apply_workers_` for worker ownership.
- Keep production worker thread startup behind explicit `Start()` so tests can call deterministic methods.

- [ ] **Step 4: Run slave apply tests**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: slave apply ordering tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/repl/slave_replicator.h src/repl/slave_replicator.cpp tests/repl_test.cpp
git commit -m "repl: add slave apply ordering"
```

### Task 10: Libco Network Server And Main Binary

**Files:**
- Create: `src/net/server.h`
- Create: `src/net/server.cpp`
- Create: `src/main.cpp`
- Create: `tests/integration_resp_test.cpp`

- [ ] **Step 1: Write integration smoke test**

Create `tests/integration_resp_test.cpp`:

```cpp
#include "test_harness.h"

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/resp_codec.h"

CACHE_TEST(RespPipelineExecutesThroughDispatcher) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  protocol::RespCodec codec;
  codec.AppendBytes("*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n");

  std::string out;
  while (auto command = codec.NextCommand()) {
    protocol::PackResponse(dispatcher.Execute(command->args, engine, 1000), &out);
  }

  test::RequireEqual(out, "+OK\r\n$1\r\n1\r\n", "pipeline response");
}
```

- [ ] **Step 2: Run integration smoke test**

Run: `cmake --build build -j && ./build/cache_tests`

Expected: integration smoke test passes before socket networking is added.

- [ ] **Step 3: Implement libco server wrapper**

Create `src/net/server.h`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"

namespace net {

struct ServerConfig {
  std::string host = "0.0.0.0";
  std::uint16_t port = 6379;
  int worker_count = 4;
  int coroutine_count_per_worker = 1024;
};

class Server {
 public:
  Server(ServerConfig config, cache::CacheEngine* engine);
  int Run();

 private:
  ServerConfig config_;
  cache::CacheEngine* engine_;
  command::CommandDispatcher dispatcher_;
};

}  // namespace net
```

Implement `server.cpp` by adapting `thirdparty/libco/example/example_echosvr.cpp`:

- Main accept coroutine accepts nonblocking sockets.
- Round-robin dispatches fd to worker.
- Worker has coroutine pool.
- Connection coroutine reads bytes, feeds `RespCodec`, executes commands, packs responses, and writes all bytes with `co_poll()` on `EAGAIN`.
- `SIGPIPE` is ignored in `main.cpp`.

- [ ] **Step 4: Implement CLI main**

Create `src/main.cpp`:

```cpp
#include <cstdlib>
#include <iostream>
#include <signal.h>

#include "cache/cache_engine.h"
#include "net/server.h"

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);

  net::ServerConfig config;
  if (argc >= 2) {
    config.port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  }
  if (argc >= 3) {
    config.worker_count = std::atoi(argv[2]);
  }
  if (argc >= 4) {
    config.coroutine_count_per_worker = std::atoi(argv[3]);
  }

  cache::CacheEngine engine;
  net::Server server(config, &engine);
  return server.Run();
}
```

- [ ] **Step 5: Run server smoke test manually**

Run: `cmake --build build -j`

In one terminal: `./build/cache_server 6380 2 128`

In another terminal:

```bash
printf '*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n' | nc 127.0.0.1 6380
```

Expected output:

```text
+OK
$1
1
```

The literal RESP bytes are `+OK\r\n$1\r\n1\r\n`.

- [ ] **Step 6: Commit**

```bash
git add src/net/server.h src/net/server.cpp src/main.cpp tests/integration_resp_test.cpp
git commit -m "net: add libco redis protocol server"
```

### Task 11: Benchmark Driver And Final Verification

**Files:**
- Create: `bench/run_single_worker_qps.sh`
- Modify: `docs/superpowers/plans/2026-05-20-cache-server-implementation.md`

- [ ] **Step 1: Add benchmark script**

Create `bench/run_single_worker_qps.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

port="${1:-6380}"
requests="${2:-10000}"

if ! command -v redis-benchmark >/dev/null 2>&1; then
  echo "redis-benchmark not installed; skipping benchmark"
  exit 0
fi

redis-benchmark -h 127.0.0.1 -p "${port}" -n "${requests}" -t set,get -P 64 -q
```

Run: `chmod +x bench/run_single_worker_qps.sh`

- [ ] **Step 2: Run full verification**

Run:

```bash
cmake -S . -B build
cmake --build build -j
cd build && ctest --output-on-failure
```

Expected: build succeeds and `cache_tests` passes.

- [ ] **Step 3: Run optional thread sanitizer build**

Run:

```bash
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan -j
cd build-tsan && ctest --output-on-failure
```

Expected: tests pass without thread sanitizer reports.

- [ ] **Step 4: Commit**

```bash
git add bench/run_single_worker_qps.sh docs/superpowers/plans/2026-05-20-cache-server-implementation.md
git commit -m "test: add cache server benchmark driver"
```

## Self-Review

Spec coverage:

- RESP parsing and packing are covered by Task 2.
- C++17 object boundaries and polymorphic `ExecCmd` are covered by Tasks 4-6.
- Data structure command files are covered by Tasks 4-6.
- Slot locks, immutable map publication, binlog sequence, and ACK cleanup are covered by Tasks 3 and 8.
- Microsecond TTL and single-threaded expiration are covered by Task 7.
- Custom RESP replication, resume, slave apply ordering, and 50 ms ACK semantics are covered by Tasks 8-9.
- Libco worker model is covered by Task 10.
- Benchmarks and final verification are covered by Task 11.

Execution notes:

- Keep each task on its own commit.
- Do not change `thirdparty/*`.
- Keep `HashSlot` internals private; tests must use public methods.
- Keep command business logic in the `*_cmd.cpp` files, not in `CommandDispatcher`.
- If a task uncovers a design defect, update the spec and this plan in the same commit as the smallest related code change.
