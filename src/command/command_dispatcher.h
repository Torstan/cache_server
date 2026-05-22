#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cache/cache_engine.h"
#include "command/redis_cmd.h"

namespace command {

class CommandDispatcher {
 public:
  CommandDispatcher();

  protocol::Response Execute(const std::vector<std::string>& args,
                             cache::CacheEngine& engine,
                             std::uint64_t now_us) const;
  protocol::Response Execute(const std::vector<std::string_view>& args,
                             cache::CacheEngine& engine,
                             std::uint64_t now_us) const;

 private:
  struct CommandEntry {
    RedisCmd* cmd;
  };

  std::unordered_map<std::string, CommandEntry> commands_;
};

}  // namespace command
