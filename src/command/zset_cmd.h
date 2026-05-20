#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "command/redis_cmd.h"

namespace command {

class ZAddCmd : public RedisCmd {
 public:
  ZAddCmd(std::string_view key, double score, std::string_view member);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  double score_;
  std::string member_;
};

class ZScoreCmd : public RedisCmd {
 public:
  ZScoreCmd(std::string_view key, std::string_view member);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::string member_;
};

}  // namespace command
