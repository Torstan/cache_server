#pragma once

#include "command/redis_cmd.h"

namespace command {

class ExistsCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class TypeCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class PTtlCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

class DelCmd : public RedisCmd {
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

class ExpireCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args,
      const CommandReplayOptions& replay_options) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
  CommandResult ExecWithResult(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us,
      const CommandReplayOptions& replay_options) const override;
};

class TtlCmd : public RedisCmd {
 public:
  std::optional<std::string> CheckArity(
      const std::vector<std::string_view>& args) const override;
  protocol::Response ExecCmd(
      const std::vector<std::string_view>& args, cache::CacheEngine& engine,
      std::uint64_t now_us) const override;
};

}  // namespace command
