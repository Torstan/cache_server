# Redis Command Interface Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor Redis command execution to use stateless, reusable command objects with O(1) dispatch and generic CacheEngine interface

**Architecture:** Three-layer design with command layer (stateless commands, validation), storage layer (generic RedisObject operations), and utility layer (shared parsing functions). Commands registered in map at startup, eliminating per-request allocations.

**Tech Stack:** C++17, CMake, CTest, existing immutable container library

---

## File Structure

**New Files:**
- `src/common/parse_utils.h` - Parsing utilities (ParseInt64, ParseFiniteDouble, ToUpperAscii)
- `src/common/parse_utils.cpp` - Implementation of parsing utilities
- `tests/parse_utils_test.cpp` - Unit tests for parsing utilities
- `tests/cache_engine_generic_test.cpp` - Tests for new CacheEngine generic interface

**Modified Files:**
- `src/cache/cache_engine.h` - Add generic Get/Set/Update interfaces
- `src/cache/cache_engine.cpp` - Implement generic interfaces
- `src/command/redis_cmd.h` - Update interface with CheckArity and new ExecCmd signature
- `src/command/command_dispatcher.h` - Add command registry
- `src/command/command_dispatcher.cpp` - Implement registry-based dispatch
- `src/command/string_cmd.h` - Remove member variables, update interface
- `src/command/string_cmd.cpp` - Implement stateless commands
- `src/command/hash_cmd.h` - Remove member variables, update interface
- `src/command/hash_cmd.cpp` - Implement stateless commands
- `src/command/set_cmd.h` - Remove member variables, update interface
- `src/command/set_cmd.cpp` - Implement stateless commands
- `src/command/zset_cmd.h` - Remove member variables, update interface
- `src/command/zset_cmd.cpp` - Implement stateless commands
- `src/command/key_cmd.h` - Remove member variables, update interface
- `src/command/key_cmd.cpp` - Implement stateless commands
- `tests/cache_command_test.cpp` - Update tests for new interface

---

## Task 1: Create Parse Utilities

**Files:**
- Create: `src/common/parse_utils.h`
- Create: `src/common/parse_utils.cpp`
- Create: `tests/parse_utils_test.cpp`

- [ ] **Step 1: Write test for ParseInt64**

```cpp
#include "test_harness.h"
#include "common/parse_utils.h"
#include <cstdint>
#include <limits>

CACHE_TEST(ParseInt64Valid) {
  std::int64_t result = 0;
  test::Require(common::ParseInt64("123", &result), "Parse positive integer");
  test::RequireEqual(result, 123, "Correct value");
  
  test::Require(common::ParseInt64("-456", &result), "Parse negative integer");
  test::RequireEqual(result, -456, "Correct negative value");
  
  test::Require(common::ParseInt64("0", &result), "Parse zero");
  test::RequireEqual(result, 0, "Zero value");
}

CACHE_TEST(ParseInt64Invalid) {
  std::int64_t result = 0;
  test::Require(!common::ParseInt64("", &result), "Empty string fails");
  test::Require(!common::ParseInt64("abc", &result), "Non-numeric fails");
  test::Require(!common::ParseInt64("12.34", &result), "Decimal fails");
  test::Require(!common::ParseInt64("123abc", &result), "Trailing chars fail");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: Compilation error - parse_utils.h not found

- [ ] **Step 3: Write parse_utils.h header**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace common {

// Parse integer from string view
// Returns true on success, false on error
bool ParseInt64(std::string_view text, std::int64_t* out);

// Parse finite double from string view
// Returns true on success, false on error (including inf/nan)
bool ParseFiniteDouble(std::string_view text, double* out);

// Convert ASCII string to uppercase
std::string ToUpperAscii(std::string_view text);

}  // namespace common
```

- [ ] **Step 4: Write parse_utils.cpp implementation**

```cpp
#include "common/parse_utils.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace common {

bool ParseInt64(std::string_view text, std::int64_t* out) {
  if (text.empty()) {
    return false;
  }

  // Convert to null-terminated string for strtoll
  std::string str(text);
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(str.c_str(), &end, 10);
  if (errno == ERANGE || end == str.c_str() || *end != '\0') {
    return false;
  }
  static_assert(sizeof(long long) >= sizeof(std::int64_t),
                "strtoll result must hold int64");
  *out = static_cast<std::int64_t>(value);
  return true;
}

bool ParseFiniteDouble(std::string_view text, double* out) {
  if (text.empty()) {
    return false;
  }

  std::string str(text);
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(str.c_str(), &end);
  if (errno == ERANGE || end == str.c_str() || *end != '\0' ||
      !std::isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

std::string ToUpperAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    if (ch >= 'a' && ch <= 'z') {
      result.push_back(static_cast<char>(ch - 'a' + 'A'));
    } else {
      result.push_back(static_cast<char>(ch));
    }
  }
  return result;
}

}  // namespace common
```

- [ ] **Step 5: Add more tests for ParseFiniteDouble and ToUpperAscii**

```cpp
CACHE_TEST(ParseFiniteDoubleValid) {
  double result = 0.0;
  test::Require(common::ParseFiniteDouble("3.14", &result), "Parse decimal");
  test::Require(std::abs(result - 3.14) < 0.001, "Correct decimal value");
  
  test::Require(common::ParseFiniteDouble("-2.5", &result), "Parse negative");
  test::Require(std::abs(result + 2.5) < 0.001, "Correct negative value");
  
  test::Require(common::ParseFiniteDouble("0.0", &result), "Parse zero");
  test::RequireEqual(result, 0.0, "Zero value");
}

CACHE_TEST(ParseFiniteDoubleInvalid) {
  double result = 0.0;
  test::Require(!common::ParseFiniteDouble("", &result), "Empty fails");
  test::Require(!common::ParseFiniteDouble("abc", &result), "Non-numeric fails");
  test::Require(!common::ParseFiniteDouble("inf", &result), "Infinity fails");
  test::Require(!common::ParseFiniteDouble("nan", &result), "NaN fails");
}

CACHE_TEST(ToUpperAsciiWorks) {
  test::RequireEqual(common::ToUpperAscii("hello"), "HELLO", "Lowercase to upper");
  test::RequireEqual(common::ToUpperAscii("WORLD"), "WORLD", "Already upper");
  test::RequireEqual(common::ToUpperAscii("HeLLo"), "HELLO", "Mixed case");
  test::RequireEqual(common::ToUpperAscii("set123"), "SET123", "With numbers");
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test`
Expected: All parse_utils tests pass

- [ ] **Step 7: Commit parse utilities**

```bash
git add src/common/parse_utils.h src/common/parse_utils.cpp tests/parse_utils_test.cpp
git commit -m "feat: add common parse utilities

Add ParseInt64, ParseFiniteDouble, and ToUpperAscii utilities
to src/common for use across command implementations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Add CacheEngine Generic Interface

**Files:**
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`
- Create: `tests/cache_engine_generic_test.cpp`

- [ ] **Step 1: Write test for CacheEngine::Get**

```cpp
#include "test_harness.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"

CACHE_TEST(CacheEngineGetNonExistent) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;
  
  const cache::RedisObject* obj = engine.Get("missing", now_us);
  test::Require(obj == nullptr, "Get non-existent key returns nullptr");
}

CACHE_TEST(CacheEngineGetExpired) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;
  
  cache::RedisObject obj = cache::RedisObject::MakeString("value");
  obj = obj.WithDeadline(now_us + 1'000'000);
  engine.Set("key", std::move(obj), now_us);
  
  const cache::RedisObject* retrieved = engine.Get("key", now_us + 2'000'000);
  test::Require(retrieved == nullptr, "Get expired key returns nullptr");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: Compilation error - CacheEngine::Get method not found

- [ ] **Step 3: Add Get/Set/Update declarations to cache_engine.h**

```cpp
// Add to CacheEngine class in src/cache/cache_engine.h
// After the constructor, before existing methods:

  // Generic RedisObject operations
  RedisObject* Get(std::string_view key, std::uint64_t now_us);
  const RedisObject* Get(std::string_view key, std::uint64_t now_us) const;
  
  WriteResult Set(std::string_view key, RedisObject obj, std::uint64_t now_us);
  
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(RedisObject*)> updater,
      std::uint64_t now_us);
```

- [ ] **Step 4: Implement CacheEngine generic methods**

Implement Get/Set/Update in cache_engine.cpp following the design spec patterns.

- [ ] **Step 5: Add comprehensive tests**

Create tests/cache_engine_generic_test.cpp with tests for all scenarios.

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test`
Expected: All CacheEngine generic interface tests pass

- [ ] **Step 7: Commit CacheEngine generic interface**

```bash
git add src/cache/cache_engine.h src/cache/cache_engine.cpp tests/cache_engine_generic_test.cpp
git commit -m "feat: add CacheEngine generic interface

Add Get/Set/Update methods for generic RedisObject operations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Update RedisCmd Interface

**Files:**
- Modify: `src/command/redis_cmd.h`

- [ ] **Step 1: Update redis_cmd.h with new interface**

Add CheckArity method and update ExecCmd signature to accept args vector.

- [ ] **Step 2: Commit RedisCmd interface update**

```bash
git add src/command/redis_cmd.h
git commit -m "refactor: update RedisCmd interface

Add CheckArity and update ExecCmd to accept args vector.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Refactor String Commands

**Files:**
- Modify: `src/command/string_cmd.h`
- Modify: `src/command/string_cmd.cpp`

- [ ] **Step 1: Remove member variables**

Update string_cmd.h to remove key/value member variables from SetCmd and GetCmd.

- [ ] **Step 2: Implement CheckArity and ExecCmd for SetCmd**

CheckArity validates args.size() == 3, ExecCmd uses CacheEngine::Set.

- [ ] **Step 3: Implement CheckArity and ExecCmd for GetCmd**

CheckArity validates args.size() == 2, ExecCmd uses CacheEngine::Get.

- [ ] **Step 4: Commit string command refactoring**

```bash
git add src/command/string_cmd.h src/command/string_cmd.cpp
git commit -m "refactor: make string commands stateless

Remove member variables, use CacheEngine generic interface.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Refactor Hash Commands

**Files:**
- Modify: `src/command/hash_cmd.h`
- Modify: `src/command/hash_cmd.cpp`

- [ ] **Step 1: Remove member variables**

Update hash_cmd.h to remove member variables from HSetCmd and HGetCmd.

- [ ] **Step 2: Implement CheckArity and ExecCmd for HSetCmd**

Use CacheEngine::Update with lambda for atomic hash insertion.

- [ ] **Step 3: Implement CheckArity and ExecCmd for HGetCmd**

Use CacheEngine::Get, check type, lookup field.

- [ ] **Step 4: Commit hash command refactoring**

```bash
git add src/command/hash_cmd.h src/command/hash_cmd.cpp
git commit -m "refactor: make hash commands stateless

Use CacheEngine::Update for atomic hash operations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Refactor Set Commands

**Files:**
- Modify: `src/command/set_cmd.h`
- Modify: `src/command/set_cmd.cpp`

- [ ] **Step 1: Remove member variables**

Update set_cmd.h to remove member variables.

- [ ] **Step 2: Implement CheckArity and ExecCmd**

Use CacheEngine::Update for set operations.

- [ ] **Step 3: Commit set command refactoring**

```bash
git add src/command/set_cmd.h src/command/set_cmd.cpp
git commit -m "refactor: make set commands stateless

Use CacheEngine generic interface for set operations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Refactor ZSet Commands

**Files:**
- Modify: `src/command/zset_cmd.h`
- Modify: `src/command/zset_cmd.cpp`

- [ ] **Step 1: Remove member variables**

Update zset_cmd.h to remove member variables.

- [ ] **Step 2: Implement CheckArity and ExecCmd**

Use common::ParseFiniteDouble and CacheEngine::Update.

- [ ] **Step 3: Commit zset command refactoring**

```bash
git add src/command/zset_cmd.h src/command/zset_cmd.cpp
git commit -m "refactor: make zset commands stateless

Use common::ParseFiniteDouble and CacheEngine generic interface.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Refactor Key Commands

**Files:**
- Modify: `src/command/key_cmd.h`
- Modify: `src/command/key_cmd.cpp`

- [ ] **Step 1: Remove member variables**

Update key_cmd.h to remove member variables.

- [ ] **Step 2: Implement CheckArity and ExecCmd**

Use common::ParseInt64 for ExpireCmd, use CacheEngine methods.

- [ ] **Step 3: Commit key command refactoring**

```bash
git add src/command/key_cmd.h src/command/key_cmd.cpp
git commit -m "refactor: make key commands stateless

Use common::ParseInt64 and CacheEngine generic interface.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Refactor CommandDispatcher

**Files:**
- Modify: `src/command/command_dispatcher.h`
- Modify: `src/command/command_dispatcher.cpp`

- [ ] **Step 1: Add CommandEntry struct and registry to header**

Add struct and unordered_map member to command_dispatcher.h.

- [ ] **Step 2: Update Execute signature**

Change to accept vector<string_view> instead of vector<string>.

- [ ] **Step 3: Implement command registry in constructor**

Create static command instances and register them in commands_ map.

- [ ] **Step 4: Implement new Execute method**

Lookup command, call CheckArity, call ExecCmd.

- [ ] **Step 5: Remove old Build method**

Delete Build method and ErrorCmd class.

- [ ] **Step 6: Update ToUpperAscii usage**

Use common::ToUpperAscii instead of local function.

- [ ] **Step 7: Verify compilation**

Run: `make build`
Expected: CommandDispatcher compiles successfully

- [ ] **Step 8: Commit CommandDispatcher refactoring**

```bash
git add src/command/command_dispatcher.h src/command/command_dispatcher.cpp
git commit -m "refactor: implement registry-based command dispatch

Replace if-else chain with O(1) map lookup.
Commands registered at startup, eliminating per-request allocations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Update Tests

**Files:**
- Modify: `tests/cache_command_test.cpp`

- [ ] **Step 1: Update test to use CommandDispatcher::Execute**

Replace direct command construction with Execute calls.

- [ ] **Step 2: Add tests for error cases**

Test empty command, unknown command, arity errors.

- [ ] **Step 3: Run all tests**

Run: `make test`
Expected: All tests pass

- [ ] **Step 4: Commit test updates**

```bash
git add tests/cache_command_test.cpp
git commit -m "test: update tests for new command interface

Use CommandDispatcher::Execute, add error case tests.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 11: Remove Old CacheEngine Methods

**Files:**
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`

- [ ] **Step 1: Remove old method declarations**

Delete GetString, SetString, HSet, HGet, SAdd, SIsMember, ZAdd, ZScore.

- [ ] **Step 2: Remove old method implementations**

Delete implementations from cache_engine.cpp.

- [ ] **Step 3: Verify compilation**

Run: `make build`
Expected: Build succeeds (all commands now use generic interface)

- [ ] **Step 4: Run all tests**

Run: `make test`
Expected: All tests pass

- [ ] **Step 5: Commit cleanup**

```bash
git add src/cache/cache_engine.h src/cache/cache_engine.cpp
git commit -m "refactor: remove old command-specific CacheEngine methods

All commands now use generic Get/Set/Update interface.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 12: Performance Validation

**Files:**
- None (validation only)

- [ ] **Step 1: Run benchmark**

Run: `make bench`
Expected: Performance equal or better than baseline

- [ ] **Step 2: Check memory allocations**

Use profiling tools to verify no per-request command allocations.

- [ ] **Step 3: Run extended stress test**

Run benchmark for 10+ minutes to check for memory leaks.

- [ ] **Step 4: Document results**

Record performance metrics in commit message or docs.

---

## Completion Checklist

- [ ] All 11 commands refactored to stateless
- [ ] All tests passing
- [ ] No old CacheEngine methods remain
- [ ] Performance validated
- [ ] Code compiles without warnings
- [ ] Git history is clean with descriptive commits

---

## Rollback Plan

If critical issues discovered:

1. Identify the problematic commit
2. Use `git revert <commit>` to undo specific changes
3. Or use `git reset --hard <commit>` to rollback to before refactoring (only if no other work depends on these changes)
4. Investigate root cause
5. Fix and re-apply

---

## Expected Outcomes

**Performance:**
- 10-20% throughput improvement from eliminated allocations
- O(1) command lookup vs O(n) if-else chain
- Better CPU cache locality

**Code Quality:**
- Clear separation of concerns
- Uniform command interface
- Easier to add new commands
- Better testability

**Maintainability:**
- Reduced code duplication
- Consistent error handling
- Self-documenting architecture

