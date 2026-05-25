#include "command/key_cmd.h"

#include <limits>
#include <string>

#include "common/parse_utils.h"

namespace command {
namespace {

constexpr std::uint64_t kMicrosPerSecond = 1000000ULL;
constexpr std::uint64_t kMicrosPerMillisecond = 1000ULL;

std::int64_t RelativeSecondsFromTtl(std::uint64_t remaining_ttl_us) {
  const std::uint64_t rounded =
      remaining_ttl_us / kMicrosPerSecond +
      (remaining_ttl_us % kMicrosPerSecond == 0 ? 0 : 1);
  if (rounded >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(rounded);
}

std::int64_t RelativeMillisecondsFromTtl(std::uint64_t remaining_ttl_us) {
  const std::uint64_t milliseconds = remaining_ttl_us / kMicrosPerMillisecond;
  if (milliseconds >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(milliseconds);
}

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

}  // namespace

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
      RelativeMillisecondsFromTtl(object->DeadlineUs() - now_us));
}

std::optional<std::string> DelCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'del' command";
  }
  return std::nullopt;
}

protocol::Response DelCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult DelCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto deleted = engine.Del(args[1], now_us).changed;
  return CommandResult{protocol::Response::Integer(deleted ? 1 : 0)};
}

std::optional<std::string> ExpireCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'expire' command";
  }
  return std::nullopt;
}

std::optional<std::string> ExpireCmd::CheckArity(
    const std::vector<std::string_view>& args,
    const CommandReplayOptions& replay_options) const {
  if (replay_options.remaining_ttl_us > 0 &&
      (args.size() == 2 || args.size() == 3)) {
    return std::nullopt;
  }
  return CheckArity(args);
}

protocol::Response ExpireCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult ExpireCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us, {});
}

CommandResult ExpireCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us,
    const CommandReplayOptions& replay_options) const {
  std::int64_t seconds = 0;
  if (replay_options.remaining_ttl_us > 0) {
    seconds = RelativeSecondsFromTtl(replay_options.remaining_ttl_us);
  } else if (!common::ParseInt64(args[2], &seconds)) {
    return CommandResult{
        protocol::Response::Error("ERR value is not an integer or out of range")};
  }
  bool ok = engine.Expire(args[1], seconds, now_us);
  return CommandResult{protocol::Response::Integer(ok ? 1 : 0)};
}

std::optional<std::string> TtlCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'ttl' command";
  }
  return std::nullopt;
}

protocol::Response TtlCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return protocol::Response::Integer(engine.Ttl(args[1], now_us));
}

}  // namespace command
