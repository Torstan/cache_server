#pragma once

#include <cstdint>

#include "cache/cache_engine.h"
#include "protocol/response.h"

namespace command {

class RedisCmd {
 public:
  virtual ~RedisCmd() = default;
  virtual protocol::Response ExecCmd(cache::CacheEngine& engine,
                                     std::uint64_t now_us) const = 0;
};

}  // namespace command
