#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "command/redis_cmd.h"

namespace command {

class HSetCmd : public RedisCmd {
 public:
  HSetCmd(std::string_view key, std::string_view field,
          std::string_view value);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::string field_;
  std::string value_;
};

class HGetCmd : public RedisCmd {
 public:
  HGetCmd(std::string_view key, std::string_view field);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::string field_;
};

}  // namespace command
