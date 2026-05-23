#include "command/string_cmd.h"

#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

std::optional<std::string> SetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'set' command";
  }
  return std::nullopt;
}

protocol::Response SetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SetCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", std::string(args[1]), std::string(args[2])};
  engine.Set(args[1], cache::RedisObject::MakeString(args[2]),
             std::move(record), now_us);
  return CommandResult{protocol::Response::SimpleString("OK")};
}

std::optional<std::string> GetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'get' command";
  }
  return std::nullopt;
}

protocol::Response GetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) {
    return protocol::Response::NullBulk();
  }
  if (obj->Type() != cache::RedisObjectType::kString) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const cache::PackedString* value = obj->StringValue();
  if (value == nullptr) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::BulkString(value->ToString());
}

}  // namespace command
