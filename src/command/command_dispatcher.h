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
  bool IsWriteCommand(std::string_view command) const;
  protocol::Response ExecuteReadOnly(
      const std::vector<std::string>& args, cache::CacheEngine& engine,
      std::uint64_t now_us, const std::function<bool(std::size_t)>& can_read) const;

 private:
  struct CommandEntry {
    RedisCmd* cmd;
    bool write_cmd = false;
    bool multi_key = false;
  };

  std::unordered_map<std::string, CommandEntry> commands_;
};

}  // namespace command
