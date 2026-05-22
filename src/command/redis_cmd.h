#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "protocol/response.h"

namespace command {

struct CommandResult {
  protocol::Response response;
  bool wrote = false;
};

class RedisCmd {
 public:
  virtual ~RedisCmd() = default;

  virtual std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const = 0;

  virtual protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const = 0;

  virtual CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const;
};

}  // namespace command
