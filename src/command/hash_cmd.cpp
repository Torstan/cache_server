#include "command/hash_cmd.h"

#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

std::optional<std::string> HSetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) {
    return "ERR wrong number of arguments for 'hset' command";
  }
  return std::nullopt;
}

protocol::Response HSetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult HSetCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::string_view key = args[1], field = args[2], value = args[3];
  bool created = false;
  bool wrong_type = false;
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kHSet;
  record.args = {"HSET", std::string(key), std::string(field),
                 std::string(value)};

  engine.Update(
      key,
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
          created = hash.Find(cache::PackedString(field)) == nullptr;
        } else {
          created = true;
        }
        cache::HashValue next =
            hash.Set(cache::PackedString(field), cache::PackedString(value));
        cache::RedisObject obj = cache::RedisObject::MakeHash(std::move(next));
        return deadline_us ? obj.WithDeadline(deadline_us) : obj;
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(created ? 1 : 0)};
}

std::optional<std::string> HGetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'hget' command";
  }
  return std::nullopt;
}

protocol::Response HGetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) {
    return protocol::Response::NullBulk();
  }
  if (obj->Type() != cache::RedisObjectType::kHash) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const cache::HashValue* hash = obj->Hash();
  if (hash == nullptr) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const cache::PackedString* found =
      hash->Find(cache::PackedString(args[2]));
  if (!found) {
    return protocol::Response::NullBulk();
  }
  return protocol::Response::BulkString(found->ToString());
}

}  // namespace command
