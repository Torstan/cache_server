#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cache/cache_engine.h"
#include "command/redis_cmd.h"

namespace command {

class CommandDispatcher {
 public:
  protocol::Response Execute(const std::vector<std::string>& args,
                             cache::CacheEngine& engine,
                             std::uint64_t now_us) const;

 private:
  std::unique_ptr<RedisCmd> Build(const std::vector<std::string>& args) const;
};

}  // namespace command
