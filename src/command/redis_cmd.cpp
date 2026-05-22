#include "command/redis_cmd.h"

namespace command {

std::optional<std::string> RedisCmd::CheckArity(
    const std::vector<std::string_view>& args,
    const CommandReplayOptions& /*replay_options*/) const {
  return CheckArity(args);
}

CommandResult RedisCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return CommandResult{ExecCmd(args, engine, now_us), false};
}

CommandResult RedisCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us,
    const CommandReplayOptions& /*replay_options*/) const {
  return ExecWithResult(args, engine, now_us);
}

}  // namespace command
