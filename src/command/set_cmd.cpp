#include "command/set_cmd.h"

#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

std::optional<std::string> SAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'sadd' command";
  }
  return std::nullopt;
}

protocol::Response SAddCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SAddCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::string_view key = args[1], member = args[2];
  bool added = false;
  bool wrong_type = false;
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSAdd;
  record.args = {"SADD", std::string(key), std::string(member)};

  cache::WriteResult result = engine.Update(
      key,
      [&](std::optional<cache::RedisObject> existing)
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
            return std::nullopt;
          }
        }
        added = true;
        cache::SetValue next = set.Add(cache::PackedString(member));
        cache::RedisObject obj = cache::RedisObject::MakeSet(std::move(next));
        return deadline_us ? obj.WithDeadline(deadline_us) : obj;
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError), false};
  }
  return CommandResult{protocol::Response::Integer(added ? 1 : 0),
                       result.changed};
}

std::optional<std::string> SIsMemberCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'sismember' command";
  }
  return std::nullopt;
}

protocol::Response SIsMemberCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) {
    return protocol::Response::Integer(0);
  }
  if (obj->Type() != cache::RedisObjectType::kSet) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const cache::SetValue* set = obj->Set();
  if (set == nullptr) {
    return protocol::Response::Error(kWrongTypeError);
  }
  bool is_member = set->Contains(cache::PackedString(args[2]));
  return protocol::Response::Integer(is_member ? 1 : 0);
}

}  // namespace command
