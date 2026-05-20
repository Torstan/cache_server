#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "command/redis_cmd.h"

namespace command {

class SAddCmd : public RedisCmd {
 public:
  SAddCmd(std::string_view key, std::string_view member);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::string member_;
};

class SIsMemberCmd : public RedisCmd {
 public:
  SIsMemberCmd(std::string_view key, std::string_view member);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::string member_;
};

}  // namespace command
