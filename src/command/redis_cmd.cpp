#include "command/redis_cmd.h"

namespace command {

CommandResult RedisCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return CommandResult{ExecCmd(args, engine, now_us), false};
}

}  // namespace command
