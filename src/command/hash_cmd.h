#pragma once

#include "command/redis_cmd.h"

namespace command {

class HSetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class HGetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
