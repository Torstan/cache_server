#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "command/redis_cmd.h"

namespace command {

class DelCmd : public RedisCmd {
 public:
  explicit DelCmd(std::string_view key);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
};

class ExpireCmd : public RedisCmd {
 public:
  ExpireCmd(std::string_view key, std::int64_t seconds);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
  std::int64_t seconds_ = 0;
};

class TtlCmd : public RedisCmd {
 public:
  explicit TtlCmd(std::string_view key);
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override;

 private:
  std::string key_;
};

}  // namespace command
