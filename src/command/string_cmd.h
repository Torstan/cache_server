#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "command/redis_cmd.h"

namespace command {

class SetCmd : public RedisCmd {
 public:
  SetCmd(std::string_view key, std::string_view value);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::string value_;
};

class GetCmd : public RedisCmd {
 public:
  explicit GetCmd(std::string_view key);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
};

}  // namespace command
