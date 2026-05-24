#include "command/hash_cmd.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "cache/binlog.h"
#include "cache/redis_object.h"
#include "common/parse_utils.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr const char* kIntegerError =
    "ERR value is not an integer or out of range";
constexpr const char* kOverflowError =
    "ERR increment or decrement would overflow";

std::string WrongArity(const char* command) {
  return std::string("ERR wrong number of arguments for '") + command +
         "' command";
}

std::optional<std::string> CheckExactArity(
    const std::vector<std::string_view>& args, std::size_t expected,
    const char* command) {
  if (args.size() != expected) {
    return WrongArity(command);
  }
  return std::nullopt;
}

std::optional<std::string> CheckMinArity(
    const std::vector<std::string_view>& args, std::size_t minimum,
    const char* command) {
  if (args.size() < minimum) {
    return WrongArity(command);
  }
  return std::nullopt;
}

std::optional<std::string> CheckFieldValueArity(
    const std::vector<std::string_view>& args, const char* command) {
  if (args.size() < 4 || args.size() % 2 != 0) {
    return WrongArity(command);
  }
  return std::nullopt;
}

cache::BinlogRecord MakeRecord(const std::vector<std::string_view>& args,
                               std::string_view command,
                               std::optional<cache::BinlogOp> op =
                                   std::nullopt) {
  cache::BinlogRecord record;
  record.op = op;
  record.args.reserve(args.size());
  record.args.emplace_back(command);
  for (std::size_t index = 1; index < args.size(); ++index) {
    record.args.emplace_back(args[index]);
  }
  return record;
}

cache::RedisObject HashObjectWithDeadline(cache::HashValue hash,
                                          std::uint64_t deadline_us) {
  cache::RedisObject obj = cache::RedisObject::MakeHash(std::move(hash));
  return deadline_us ? obj.WithDeadline(deadline_us) : obj;
}

const cache::HashValue* HashOrWrongType(
    const std::optional<cache::RedisObject>& obj, bool* wrong_type) {
  if (!obj) {
    return nullptr;
  }
  if (obj->Type() != cache::RedisObjectType::kHash) {
    *wrong_type = true;
    return nullptr;
  }
  const cache::HashValue* hash = obj->Hash();
  if (hash == nullptr) {
    *wrong_type = true;
  }
  return hash;
}

std::int64_t ApplyFieldValuePairs(
    const std::vector<std::string_view>& args, cache::HashValue* hash) {
  std::int64_t added = 0;
  for (std::size_t index = 2; index < args.size(); index += 2) {
    const cache::PackedString field(args[index]);
    if (hash->Find(field) == nullptr) {
      ++added;
    }
    *hash = hash->Set(field, cache::PackedString(args[index + 1]));
  }
  return added;
}

bool AddWouldOverflow(std::int64_t value, std::int64_t increment) {
  if (increment > 0) {
    return value > std::numeric_limits<std::int64_t>::max() - increment;
  }
  if (increment < 0) {
    return value < std::numeric_limits<std::int64_t>::min() - increment;
  }
  return false;
}

}  // namespace

std::optional<std::string> HSetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckFieldValueArity(args, "hset");
}

protocol::Response HSetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult HSetCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t added = 0;
  bool wrong_type = false;
  cache::BinlogRecord record = MakeRecord(args, "HSET", cache::BinlogOp::kHSet);

  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
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
        }
        added = ApplyFieldValuePairs(args, &hash);
        return HashObjectWithDeadline(std::move(hash), deadline_us);
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(added)};
}

std::optional<std::string> HGetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 3, "hget");
}

protocol::Response HGetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }
  if (hash == nullptr) {
    return protocol::Response::NullBulk();
  }
  const cache::PackedString* found = hash->Find(cache::PackedString(args[2]));
  if (!found) {
    return protocol::Response::NullBulk();
  }
  return protocol::Response::BulkString(found->ToString());
}

std::optional<std::string> HDelCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckMinArity(args, 3, "hdel");
}

protocol::Response HDelCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult HDelCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t removed = 0;
  bool wrong_type = false;
  bool delete_key = false;
  cache::BinlogRecord record = MakeRecord(args, "HDEL");

  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        if (!existing) {
          return std::nullopt;
        }
        const cache::HashValue* existing_hash = existing->Hash();
        if (existing->Type() != cache::RedisObjectType::kHash ||
            existing_hash == nullptr) {
          wrong_type = true;
          return std::nullopt;
        }

        cache::HashValue hash = *existing_hash;
        for (std::size_t index = 2; index < args.size(); ++index) {
          const cache::PackedString field(args[index]);
          if (hash.Find(field) == nullptr) {
            continue;
          }
          auto next = hash.Erase(field);
          if (!next.has_value()) {
            continue;
          }
          hash = std::move(*next);
          ++removed;
        }

        if (removed == 0) {
          return std::nullopt;
        }
        delete_key = hash.Empty();
        return HashObjectWithDeadline(std::move(hash), existing->DeadlineUs());
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (delete_key) {
    (void)engine.Del(args[1], now_us);
  }
  return CommandResult{protocol::Response::Integer(removed)};
}

std::optional<std::string> HExistsCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 3, "hexists");
}

protocol::Response HExistsCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const bool exists =
      hash != nullptr && hash->Find(cache::PackedString(args[2])) != nullptr;
  return protocol::Response::Integer(exists ? 1 : 0);
}

std::optional<std::string> HLenCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 2, "hlen");
}

protocol::Response HLenCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(
      hash == nullptr ? 0 : static_cast<std::int64_t>(hash->Size()));
}

std::optional<std::string> HStrLenCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 3, "hstrlen");
}

protocol::Response HStrLenCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }
  if (hash == nullptr) {
    return protocol::Response::Integer(0);
  }
  const cache::PackedString* found = hash->Find(cache::PackedString(args[2]));
  return protocol::Response::Integer(
      found == nullptr ? 0 : static_cast<std::int64_t>(found->Size()));
}

std::optional<std::string> HMGetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckMinArity(args, 3, "hmget");
}

protocol::Response HMGetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::vector<protocol::Response> values;
  values.reserve(args.size() - 2);
  for (std::size_t index = 2; index < args.size(); ++index) {
    const cache::PackedString* found =
        hash == nullptr ? nullptr : hash->Find(cache::PackedString(args[index]));
    if (found == nullptr) {
      values.push_back(protocol::Response::NullBulk());
    } else {
      values.push_back(protocol::Response::BulkString(found->ToString()));
    }
  }
  return protocol::Response::Array(std::move(values));
}

std::optional<std::string> HMSetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckFieldValueArity(args, "hmset");
}

protocol::Response HMSetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult HMSetCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  bool wrong_type = false;
  cache::BinlogRecord record = MakeRecord(args, "HMSET");

  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
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
        }
        (void)ApplyFieldValuePairs(args, &hash);
        return HashObjectWithDeadline(std::move(hash), deadline_us);
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::SimpleString("OK")};
}

std::optional<std::string> HGetAllCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 2, "hgetall");
}

protocol::Response HGetAllCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::vector<protocol::Response> values;
  if (hash != nullptr) {
    values.reserve(hash->Size() * 2);
    hash->ForEach([&](const cache::PackedString& field,
                      const cache::PackedString& value) {
      values.push_back(protocol::Response::BulkString(field.ToString()));
      values.push_back(protocol::Response::BulkString(value.ToString()));
    });
  }
  return protocol::Response::Array(std::move(values));
}

std::optional<std::string> HKeysCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 2, "hkeys");
}

protocol::Response HKeysCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::vector<protocol::Response> values;
  if (hash != nullptr) {
    values.reserve(hash->Size());
    hash->ForEach([&](const cache::PackedString& field,
                      const cache::PackedString&) {
      values.push_back(protocol::Response::BulkString(field.ToString()));
    });
  }
  return protocol::Response::Array(std::move(values));
}

std::optional<std::string> HValsCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 2, "hvals");
}

protocol::Response HValsCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  bool wrong_type = false;
  const cache::HashValue* hash = HashOrWrongType(obj, &wrong_type);
  if (wrong_type) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::vector<protocol::Response> values;
  if (hash != nullptr) {
    values.reserve(hash->Size());
    hash->ForEach([&](const cache::PackedString&,
                      const cache::PackedString& value) {
      values.push_back(protocol::Response::BulkString(value.ToString()));
    });
  }
  return protocol::Response::Array(std::move(values));
}

std::optional<std::string> HIncrByCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  return CheckExactArity(args, 4, "hincrby");
}

protocol::Response HIncrByCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult HIncrByCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t increment = 0;
  if (!common::ParseInt64(args[3], &increment)) {
    return CommandResult{protocol::Response::Error(kIntegerError)};
  }

  bool wrong_type = false;
  bool invalid_integer = false;
  bool overflow = false;
  std::int64_t result = 0;
  cache::BinlogRecord record = MakeRecord(args, "HINCRBY");

  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
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
        }

        std::int64_t current = 0;
        const cache::PackedString field(args[2]);
        const cache::PackedString* found = hash.Find(field);
        if (found != nullptr && !common::ParseInt64(found->View(), &current)) {
          invalid_integer = true;
          return std::nullopt;
        }
        if (AddWouldOverflow(current, increment)) {
          overflow = true;
          return std::nullopt;
        }

        result = current + increment;
        hash = hash.Set(field, cache::PackedString(std::to_string(result)));
        return HashObjectWithDeadline(std::move(hash), deadline_us);
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (invalid_integer) {
    return CommandResult{protocol::Response::Error(kIntegerError)};
  }
  if (overflow) {
    return CommandResult{protocol::Response::Error(kOverflowError)};
  }
  return CommandResult{protocol::Response::Integer(result)};
}

}  // namespace command
