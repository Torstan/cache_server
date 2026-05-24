#pragma once

#include "command/redis_cmd.h"

namespace command {

class SetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class GetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class MGetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class SetNxCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class GetSetCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class StrLenCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class AppendCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class IncrCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class DecrCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class IncrByCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class DecrByCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
