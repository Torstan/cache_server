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
- `src/cache/hash_slot.h` - Add generic Get/Set/Update interfaces
- `src/cache/hash_slot.cpp` - Implement generic interfaces
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

- [ ] **Step 1: Write parse_utils.h and parse_utils.cpp**

Create complete implementation following the design spec.

- [ ] **Step 2: Write comprehensive tests**

Create tests/parse_utils_test.cpp with tests for all three functions.

- [ ] **Step 3: Run tests**

Run: `make test`
Expected: All parse_utils tests pass

- [ ] **Step 4: Commit**

```bash
git add src/common/parse_utils.h src/common/parse_utils.cpp tests/parse_utils_test.cpp
git commit -m "feat: add common parse utilities

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Add HashSlot and CacheEngine Generic Interfaces

**Files:**
- Modify: `src/cache/hash_slot.h`
- Modify: `src/cache/hash_slot.cpp`  
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`
- Create: `tests/cache_engine_generic_test.cpp`

**Rationale:** Implement both HashSlot and CacheEngine generic interfaces together since CacheEngine delegates to HashSlot.

- [ ] **Step 1: Add HashSlot generic methods to header**

Add Get/Set/Update declarations to hash_slot.h.

- [ ] **Step 2: Implement HashSlot generic methods**

Implement in hash_slot.cpp with proper locking and binlog recording.

- [ ] **Step 3: Add CacheEngine generic methods to header**

Add Get/Set/Update declarations to cache_engine.h (both const and non-const Get).

- [ ] **Step 4: Implement CacheEngine generic methods**

Implement in cache_engine.cpp by delegating to HashSlot.

- [ ] **Step 5: Write comprehensive tests**

Create tests/cache_engine_generic_test.cpp.

- [ ] **Step 6: Run tests**

Run: `make test`
Expected: All generic interface tests pass

- [ ] **Step 7: Commit**

```bash
git add src/cache/hash_slot.h src/cache/hash_slot.cpp src/cache/cache_engine.h src/cache/cache_engine.cpp tests/cache_engine_generic_test.cpp
git commit -m "feat: add generic Get/Set/Update interfaces

Add to HashSlot and CacheEngine for command-agnostic storage operations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Update RedisCmd and CommandDispatcher Together

**Files:**
- Modify: `src/command/redis_cmd.h`
- Modify: `src/command/command_dispatcher.h`
- Modify: `src/command/command_dispatcher.cpp`

**Rationale:** Update both interfaces together to maintain compilability. CommandDispatcher will support both old and new command styles during migration.

- [ ] **Step 1: Update RedisCmd interface**

Add CheckArity and new ExecCmd signature to redis_cmd.h.

- [ ] **Step 2: Add command registry to CommandDispatcher**

Add CommandEntry struct and commands_ map to command_dispatcher.h.

- [ ] **Step 3: Implement registry and new Execute method**

In constructor, create static command instances and register them. Implement new Execute that uses CheckArity.

- [ ] **Step 4: Keep old Build method temporarily**

Leave existing Build method unchanged for backward compatibility during migration.

- [ ] **Step 5: Verify compilation**

Run: `make build`
Expected: Compiles successfully (old commands still work via Build)

- [ ] **Step 6: Commit**

```bash
git add src/command/redis_cmd.h src/command/command_dispatcher.h src/command/command_dispatcher.cpp
git commit -m "refactor: add new command interface alongside old

Add CheckArity to RedisCmd and registry-based dispatch to CommandDispatcher.
Old Build method kept for backward compatibility during migration.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Refactor String Commands

**Files:**
- Modify: `src/command/string_cmd.h`
- Modify: `src/command/string_cmd.cpp`

- [ ] **Step 1: Update string_cmd.h**

Remove member variables, add CheckArity and new ExecCmd declarations.

- [ ] **Step 2: Implement SetCmd**

CheckArity validates args.size() == 3. ExecCmd uses CacheEngine::Set.

- [ ] **Step 3: Implement GetCmd**

CheckArity validates args.size() == 2. ExecCmd uses CacheEngine::Get with type check.

- [ ] **Step 4: Verify compilation and tests**

Run: `make test`
Expected: String command tests pass

- [ ] **Step 5: Commit**

```bash
git add src/command/string_cmd.h src/command/string_cmd.cpp
git commit -m "refactor: make string commands stateless

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5-8: Refactor Remaining Commands

Apply same pattern as Task 4 to hash, set, zset, and key commands.

**Task 5: Hash Commands** (hash_cmd.h/cpp)
**Task 6: Set Commands** (set_cmd.h/cpp)  
**Task 7: ZSet Commands** (zset_cmd.h/cpp) - use common::ParseFiniteDouble
**Task 8: Key Commands** (key_cmd.h/cpp) - use common::ParseInt64

Each task follows same steps: update header, implement CheckArity/ExecCmd, test, commit.

---

## Task 9: Remove Old Interfaces

**Files:**
- Modify: `src/command/command_dispatcher.cpp`
- Modify: `src/cache/cache_engine.h`
- Modify: `src/cache/cache_engine.cpp`
- Modify: `tests/cache_command_test.cpp`

- [ ] **Step 1: Remove CommandDispatcher::Build**

Delete Build method and ErrorCmd class from command_dispatcher.cpp.

- [ ] **Step 2: Remove old CacheEngine methods**

Delete GetString, SetString, HSet, HGet, SAdd, SIsMember, ZAdd, ZScore from cache_engine.h/cpp.

- [ ] **Step 3: Update tests**

Modify cache_command_test.cpp to use CommandDispatcher::Execute instead of direct command construction.

- [ ] **Step 4: Run all tests**

Run: `make test`
Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add src/command/command_dispatcher.cpp src/cache/cache_engine.h src/cache/cache_engine.cpp tests/cache_command_test.cpp
git commit -m "refactor: remove old command interfaces

All commands now use stateless pattern with generic CacheEngine interface.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Performance Validation

- [ ] **Step 1: Run benchmark**

Run: `make bench`
Expected: Performance equal or better than baseline

- [ ] **Step 2: Document results**

Record metrics showing reduced allocations and improved throughput.

---

## Completion Checklist

- [ ] All 11 commands refactored to stateless
- [ ] All tests passing
- [ ] No old interfaces remain
- [ ] Performance validated
- [ ] Code compiles without warnings

---

## Expected Outcomes

**Performance:** 10-20% throughput improvement, O(1) command lookup
**Code Quality:** Clear separation of concerns, uniform interface
**Maintainability:** Easier to add commands, reduced duplication
