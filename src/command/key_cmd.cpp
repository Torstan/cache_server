#include "command/key_cmd.h"

#include <limits>

#include "common/parse_utils.h"

namespace command {
namespace {

constexpr std::uint64_t kMicrosPerSecond = 1000000ULL;

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

}  // namespace

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
  auto result = engine.Del(args[1], now_us);
  return CommandResult{protocol::Response::Integer(result.changed ? 1 : 0),
                       result.changed};
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
        protocol::Response::Error("ERR value is not an integer or out of range"),
        false};
  }
  bool ok = engine.Expire(args[1], seconds, now_us);
  return CommandResult{protocol::Response::Integer(ok ? 1 : 0), ok};
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
