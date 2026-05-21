# Redis Command Interface Optimization Design

**Date**: 2026-05-22  
**Status**: Approved  
**Author**: User + Claude

## Overview

This document describes a comprehensive refactoring of the Redis command execution architecture in the cache server. The goal is to optimize performance, improve code maintainability, and establish clearer separation of concerns between the command layer and storage layer.

### Key Objectives

1. **Eliminate per-request allocations**: Make command objects stateless and reusable
2. **Simplify command dispatch**: Replace if-else chain with O(1) map lookup
3. **Clarify layer responsibilities**: Command layer handles parsing/validation, storage layer handles RedisObject operations
4. **Improve extensibility**: Make adding new commands simpler and more consistent

## Architecture Overview

### Current Architecture Problems

- **Per-request allocations**: Each command execution creates a new command object with stored parameters
- **Linear command lookup**: O(n) if-else chain in `CommandDispatcher::Build`
- **Tight coupling**: `CacheEngine` has command-specific methods (`GetString`, `HSet`, etc.)
- **Scattered validation**: Arity checking mixed with command construction logic
- **Limited extensibility**: Adding new commands requires modifying multiple places

### New Architecture

**Three-layer design with clear responsibilities**:

1. **Command Layer** (`src/command`):
   - Stateless, reusable command objects
   - Parameter validation via `CheckArity`
   - Parameter parsing and type checking
   - Calls `CacheEngine` generic interfaces

2. **Storage Layer** (`src/cache`):
   - Generic `RedisObject` operations: `Get`, `Set`, `Update`
   - No knowledge of specific commands
   - Handles concurrency and expiration

3. **Utility Layer** (`src/common`):
   - Shared parsing functions: `ParseInt64`, `ParseFiniteDouble`
   - String utilities: `ToUpperAscii`

**Execution Flow**:

```
Request → CommandDispatcher::Execute
  ↓
Lookup command in registry (O(1))
  ↓
CheckArity → returns optional<string> error
  ↓
ExecCmd(args, engine, now_us)
  ↓
Parse args → Call CacheEngine generic API
  ↓
Return Response
```

## Interface Design

### 1. RedisCmd Base Class

```cpp
// src/command/redis_cmd.h
namespace command {

class RedisCmd {
 public:
  virtual ~RedisCmd() = default;
  
  // Validate argument count
  // args[0] is command name, args[1..] are parameters
  // Returns error message if validation fails
  virtual std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const = 0;
  
  // Execute command (args already validated by CheckArity)
  virtual protocol::Response ExecCmd(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const = 0;
};

}  // namespace command
```

### 2. CommandDispatcher

```cpp
// src/command/command_dispatcher.h
namespace command {

class CommandDispatcher {
 public:
  CommandDispatcher();  // Initialize command registry
  
  protocol::Response Execute(
      const std::vector<std::string_view>& args,
      cache::CacheEngine& engine,
      std::uint64_t now_us) const;

 private:
  struct CommandEntry {
    RedisCmd* cmd;        // Pointer to static command object
    std::string name;     // For error messages
  };
  
  std::unordered_map<std::string, CommandEntry> commands_;
};

}  // namespace command
```

**Implementation**:

```cpp
// src/command/command_dispatcher.cpp
CommandDispatcher::CommandDispatcher() {
  // Static command instances (created once, reused forever)
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
  
  // Register commands
  commands_["SET"] = CommandEntry{&set_cmd, "SET"};
  commands_["GET"] = CommandEntry{&get_cmd, "GET"};
  commands_["HSET"] = CommandEntry{&hset_cmd, "HSET"};
  commands_["HGET"] = CommandEntry{&hget_cmd, "HGET"};
  commands_["SADD"] = CommandEntry{&sadd_cmd, "SADD"};
  commands_["SISMEMBER"] = CommandEntry{&sismember_cmd, "SISMEMBER"};
  commands_["ZADD"] = CommandEntry{&zadd_cmd, "ZADD"};
  commands_["ZSCORE"] = CommandEntry{&zscore_cmd, "ZSCORE"};
  commands_["DEL"] = CommandEntry{&del_cmd, "DEL"};
  commands_["EXPIRE"] = CommandEntry{&expire_cmd, "EXPIRE"};
  commands_["TTL"] = CommandEntry{&ttl_cmd, "TTL"};
}

protocol::Response CommandDispatcher::Execute(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  
  if (args.empty()) {
    return protocol::Response::Error("ERR empty command");
  }
  
  // Convert command name to uppercase
  std::string cmd_name = common::ToUpperAscii(args[0]);
  
  // Lookup command
  auto it = commands_.find(cmd_name);
  if (it == commands_.end()) {
    return protocol::Response::Error("ERR unknown command '" + cmd_name + "'");
  }
  
  const CommandEntry& entry = it->second;
  
  // Validate arity
  auto arity_error = entry.cmd->CheckArity(args);
  if (arity_error) {
    return protocol::Response::Error(*arity_error);
  }
  
  // Execute command
  return entry.cmd->ExecCmd(args, engine, now_us);
}
```

### 3. CacheEngine Generic Interface

```cpp
// src/cache/cache_engine.h
namespace cache {

class CacheEngine {
 public:
  CacheEngine();

  // Get object (returns nullptr if not found or expired)
  RedisObject* Get(std::string_view key, std::uint64_t now_us);
  const RedisObject* Get(std::string_view key, std::uint64_t now_us) const;
  
  // Set object (create or overwrite)
  WriteResult Set(std::string_view key, RedisObject obj, std::uint64_t now_us);
  
  // Update object if exists
  // updater receives current object pointer (may be nullptr)
  // returns new object or nullopt to cancel operation
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(RedisObject*)> updater,
      std::uint64_t now_us);
  
  // Delete object
  WriteResult Del(std::string_view key, std::uint64_t now_us);
  
  // Set expiration
  bool Expire(std::string_view key, std::int64_t seconds, std::uint64_t now_us);
  
  // Get TTL
  std::int64_t Ttl(std::string_view key, std::uint64_t now_us) const;
  
  // Slot management (unchanged)
  std::size_t DeleteExpiredInSlot(std::size_t slot_id, std::size_t max_keys,
                                  std::uint64_t now_us);
  HashSlot& SlotForKey(std::string_view key);
  const HashSlot& SlotForKey(std::string_view key) const;
  HashSlot& SlotById(std::size_t slot_id);
  const HashSlot& SlotById(std::size_t slot_id) const;
  std::size_t SlotCount() const;

 private:
  std::vector<std::unique_ptr<HashSlot>> slots_;
};

}  // namespace cache
```

### 4. Common Utilities

```cpp
// src/common/parse_utils.h
namespace common {

// Parse integer from string
bool ParseInt64(std::string_view text, std::int64_t* out);

// Parse finite double from string
bool ParseFiniteDouble(std::string_view text, double* out);

// Convert ASCII string to uppercase
std::string ToUpperAscii(std::string_view text);

}  // namespace common
```

## Command Implementation Examples

### String Commands

```cpp
// src/command/string_cmd.h
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

```cpp
// src/command/string_cmd.cpp
namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

// SetCmd implementation
std::optional<std::string> SetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {  // SET key value
    return "ERR wrong number of arguments for 'set' command";
  }
  return std::nullopt;
}

protocol::Response SetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  // args[0] = "SET", args[1] = key, args[2] = value
  RedisObject obj = RedisObject::MakeString(args[2]);
  engine.Set(args[1], std::move(obj), now_us);
  return protocol::Response::SimpleString("OK");
}

// GetCmd implementation
std::optional<std::string> GetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {  // GET key
    return "ERR wrong number of arguments for 'get' command";
  }
  return std::nullopt;
}

protocol::Response GetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  const RedisObject* obj = engine.Get(args[1], now_us);
  if (!obj) {
    return protocol::Response::NullBulk();
  }
  if (obj->Type() != RedisObjectType::kString) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const PackedString* value = obj->StringValue();
  return protocol::Response::BulkString(std::string_view(*value));
}

}  // namespace command
```

### Hash Commands

```cpp
// HSetCmd example
std::optional<std::string> HSetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) {  // HSET key field value
    return "ERR wrong number of arguments for 'hset' command";
  }
  return std::nullopt;
}

protocol::Response HSetCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::string_view key = args[1];
  std::string_view field = args[2];
  std::string_view value = args[3];
  
  bool created = false;
  auto result = engine.Update(key, [&](RedisObject* obj) -> std::optional<RedisObject> {
    if (!obj) {
      // Create new hash
      created = true;
      HashValue hash;
      hash = hash.Insert(PackedString(field), PackedString(value));
      return RedisObject::MakeHash(std::move(hash));
    }
    
    if (obj->Type() != RedisObjectType::kHash) {
      return std::nullopt;  // Type error
    }
    
    // Update existing hash
    const HashValue* old_hash = obj->Hash();
    HashValue new_hash = old_hash->Insert(PackedString(field), PackedString(value));
    created = !old_hash->Contains(PackedString(field));
    return RedisObject::MakeHash(std::move(new_hash));
  }, now_us);
  
  if (result.status != Status::kOk) {
    return protocol::Response::Error(kWrongTypeError);
  }
  
  return protocol::Response::Integer(created ? 1 : 0);
}
```

### ZSet Commands with Parsing

```cpp
// ZAddCmd example
std::optional<std::string> ZAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) {  // ZADD key score member
    return "ERR wrong number of arguments for 'zadd' command";
  }
  return std::nullopt;
}

protocol::Response ZAddCmd::ExecCmd(
    const std::vector<std::string_view>& args,
    cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::string_view key = args[1];
  std::string_view score_str = args[2];
  std::string_view member = args[3];
  
  // Parse score
  double score = 0.0;
  if (!common::ParseFiniteDouble(score_str, &score)) {
    return protocol::Response::Error("ERR value is not a valid float");
  }
  
  bool created = false;
  auto result = engine.Update(key, [&](RedisObject* obj) -> std::optional<RedisObject> {
    if (!obj) {
      created = true;
      ZSetValue zset;
      zset = zset.Insert(PackedString(member), score);
      return RedisObject::MakeZSet(std::move(zset));
    }
    
    if (obj->Type() != RedisObjectType::kZSet) {
      return std::nullopt;
    }
    
    const ZSetValue* old_zset = obj->ZSet();
    ZSetValue new_zset = old_zset->Insert(PackedString(member), score);
    created = !old_zset->Contains(PackedString(member));
    return RedisObject::MakeZSet(std::move(new_zset));
  }, now_us);
  
  if (result.status != Status::kOk) {
    return protocol::Response::Error(kWrongTypeError);
  }
  
  return protocol::Response::Integer(created ? 1 : 0);
}
```

## Error Handling

### Error Categories

1. **Empty Command**:
   - Detected in: `CommandDispatcher::Execute`
   - Response: `"ERR empty command"`

2. **Unknown Command**:
   - Detected in: `CommandDispatcher::Execute` (map lookup fails)
   - Response: `"ERR unknown command '<cmd>'"`

3. **Arity Errors**:
   - Detected in: `RedisCmd::CheckArity`
   - Response: `"ERR wrong number of arguments for '<cmd>' command"`

4. **Type Errors**:
   - Detected in: `RedisCmd::ExecCmd` (type check fails)
   - Response: `"WRONGTYPE Operation against a key holding the wrong kind of value"`

5. **Parse Errors**:
   - Detected in: `RedisCmd::ExecCmd` (parameter parsing fails)
   - Response: Command-specific (e.g., `"ERR value is not a valid float"`)

### Edge Cases

1. **Expired Keys**:
   - `CacheEngine::Get` returns `nullptr` for expired keys
   - Commands treat as key not found

2. **Concurrent Access**:
   - Handled by `HashSlot` internal locking
   - Commands are thread-safe by design

3. **Case Insensitivity**:
   - Command names converted to uppercase before lookup
   - Uses `common::ToUpperAscii`

4. **Variable Arity Commands** (future extension):
   - `CheckArity` can validate min/max ranges
   - Current implementation uses fixed arity for simplicity

## Migration Strategy

### Approach: One-Time Refactoring

This design uses **Approach B** from the alternatives: complete refactoring in one pass. This eliminates technical debt and avoids maintaining two interfaces simultaneously.

### Migration Steps

1. **Preparation** (Day 1):
   - Create `src/common/parse_utils.h/cpp`
   - Move `ParseInt64`, `ParseFiniteDouble`, `ToUpperAscii` from `command_dispatcher.cpp`
   - Run all existing tests to establish baseline

2. **CacheEngine Refactoring** (Day 1-2):
   - Implement `Get(key, now_us) -> RedisObject*`
   - Implement `Set(key, obj, now_us) -> WriteResult`
   - Implement `Update(key, updater, now_us) -> WriteResult`
   - Keep old methods temporarily for compatibility
   - Add unit tests for new interfaces

3. **RedisCmd Interface Update** (Day 2):
   - Update `redis_cmd.h` with new interface
   - Add `CheckArity` pure virtual method
   - Update `ExecCmd` signature to accept `args`

4. **Command Migration** (Day 2-3):
   - **String commands**: `SetCmd`, `GetCmd`
   - **Hash commands**: `HSetCmd`, `HGetCmd`
   - **Set commands**: `SAddCmd`, `SIsMemberCmd`
   - **ZSet commands**: `ZAddCmd`, `ZScoreCmd`
   - **Key commands**: `DelCmd`, `ExpireCmd`, `TtlCmd`
   - Remove member variables from each command class
   - Implement stateless `CheckArity` and `ExecCmd`
   - Update unit tests for each command

5. **CommandDispatcher Refactoring** (Day 3):
   - Implement command registry in constructor
   - Update `Execute` method with new flow
   - Remove old `Build` method and if-else chain
   - Remove `ErrorCmd` class (no longer needed)

6. **Cleanup** (Day 4):
   - Delete old `CacheEngine` methods (`GetString`, `SetString`, etc.)
   - Remove any temporary compatibility code
   - Run full test suite
   - Run benchmark to verify performance

7. **Validation** (Day 4):
   - All unit tests pass
   - All integration tests pass
   - Benchmark shows equal or better performance
   - Memory profiling shows reduced allocations

### Rollback Plan

If critical issues are discovered:
1. Revert the entire commit series
2. Investigate root cause
3. Fix issues in a branch
4. Re-apply with fixes

## Testing Strategy

### Unit Tests

1. **CommandDispatcher Tests**:
   - Empty command handling
   - Unknown command handling
   - Case-insensitive command lookup
   - Arity validation integration

2. **Individual Command Tests**:
   - `CheckArity` with valid/invalid argument counts
   - `ExecCmd` with valid inputs
   - Type error handling
   - Key not found handling
   - Parse error handling (for commands with numeric args)

3. **CacheEngine Tests**:
   - `Get` with existing/missing/expired keys
   - `Set` creating new keys
   - `Set` overwriting existing keys
   - `Update` with various updater functions
   - `Update` with type mismatches

4. **Parse Utilities Tests**:
   - `ParseInt64` with valid/invalid/overflow inputs
   - `ParseFiniteDouble` with valid/invalid/infinite inputs
   - `ToUpperAscii` with mixed case strings

### Integration Tests

1. **End-to-End Command Execution**:
   - Full flow from request to response
   - Multiple commands in sequence
   - Commands on same key with different types

2. **Concurrency Tests**:
   - Multiple threads executing commands
   - Concurrent reads and writes to same key
   - Stress test with high concurrency

3. **Regression Tests**:
   - All existing functional tests must pass
   - Behavior must match pre-refactoring implementation

### Performance Validation

1. **Benchmark Metrics**:
   - Commands per second (should improve)
   - Memory allocations per command (should decrease)
   - P50/P99 latency (should be equal or better)

2. **Memory Profiling**:
   - Verify no command object allocations during execution
   - Verify static command objects are reused

3. **Stress Testing**:
   - Use existing benchmark driver
   - Run for extended period (1+ hours)
   - Monitor for memory leaks or performance degradation

## Expected Benefits

### Performance Improvements

1. **Reduced Allocations**:
   - Before: One command object allocation per request
   - After: Zero allocations (reuse static objects)
   - Impact: ~10-20% throughput improvement expected

2. **Faster Command Lookup**:
   - Before: O(n) if-else chain (n = number of commands)
   - After: O(1) hash map lookup
   - Impact: Negligible for small n, but better scalability

3. **Better Cache Locality**:
   - Static command objects stay in cache
   - Less pointer chasing during execution

### Code Quality Improvements

1. **Clearer Separation of Concerns**:
   - Command layer: parsing, validation, orchestration
   - Storage layer: data operations, concurrency, persistence
   - No cross-layer knowledge

2. **Easier to Add Commands**:
   - Before: Modify `CommandDispatcher::Build`, add if-else branch
   - After: Implement command class, add one line to registry
   - Reduced chance of merge conflicts

3. **Better Testability**:
   - Stateless commands are easier to test
   - Generic `CacheEngine` interface is easier to mock
   - Each layer can be tested independently

4. **Reduced Code Duplication**:
   - Arity checking logic centralized in `CheckArity`
   - Parse utilities shared across commands
   - Error handling patterns consistent

### Maintainability Improvements

1. **Uniform Interface**:
   - All commands follow same pattern
   - New developers can learn by example
   - Code reviews are easier

2. **Future Extensibility**:
   - Easy to add variable-arity commands
   - Easy to add command aliases
   - Easy to add command metadata (help text, flags)

3. **Reduced Coupling**:
   - `CacheEngine` doesn't know about commands
   - Commands don't know about storage internals
   - Changes in one layer don't ripple to others

## Future Enhancements

### Variable Arity Support

Extend `CheckArity` to support min/max ranges:

```cpp
class DelCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override {
    if (args.size() < 2) {  // DEL key [key ...]
      return "ERR wrong number of arguments for 'del' command";
    }
    return std::nullopt;  // No maximum
  }
  
  protocol::Response ExecCmd(...) const override {
    int deleted = 0;
    for (size_t i = 1; i < args.size(); ++i) {
      auto result = engine.Del(args[i], now_us);
      if (result.changed) deleted++;
    }
    return protocol::Response::Integer(deleted);
  }
};
```

### Command Metadata

Add metadata to `CommandEntry` for introspection:

```cpp
struct CommandEntry {
  RedisCmd* cmd;
  std::string name;
  std::string help_text;
  bool is_write_command;
  bool is_admin_command;
};
```

### Command Aliases

Support multiple names for same command:

```cpp
commands_["DEL"] = CommandEntry{&del_cmd, "DEL"};
commands_["DELETE"] = CommandEntry{&del_cmd, "DELETE"};  // Alias
```

### Batch Operations

Optimize multiple operations on same key:

```cpp
// Future: MSET key1 value1 key2 value2 ...
// Can batch multiple Set calls efficiently
```

## Conclusion

This refactoring establishes a clean, performant, and maintainable architecture for Redis command execution. The one-time migration approach eliminates technical debt and provides a solid foundation for future enhancements.

**Key Takeaways**:
- Stateless commands eliminate per-request allocations
- Generic `CacheEngine` interface clarifies layer responsibilities
- Command registry enables O(1) lookup and easy extensibility
- Comprehensive testing ensures correctness and performance

**Success Criteria**:
- All tests pass
- Performance equal or better than baseline
- Code is clearer and easier to maintain
- Adding new commands is straightforward
