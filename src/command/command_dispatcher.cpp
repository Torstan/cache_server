#include "command/command_dispatcher.h"

#include "command/hash_cmd.h"
#include "command/key_cmd.h"
#include "command/set_cmd.h"
#include "command/string_cmd.h"
#include "command/zset_cmd.h"
#include "common/parse_utils.h"

namespace command {

CommandDispatcher::CommandDispatcher() {
  static SetCmd set_cmd;
  static GetCmd get_cmd;
  static HSetCmd hset_cmd;
  static HGetCmd hget_cmd;
  static SAddCmd sadd_cmd;
  static SIsMemberCmd sismember_cmd;
  static ZAddCmd zadd_cmd;
  static ZScoreCmd zscore_cmd;
  static DelCmd del_cmd;
  static ExpireCmd expire_cmd;
  static TtlCmd ttl_cmd;

  commands_["SET"] = {&set_cmd, true};
  commands_["GET"] = {&get_cmd, false};
  commands_["HSET"] = {&hset_cmd, true};
  commands_["HGET"] = {&hget_cmd, false};
  commands_["SADD"] = {&sadd_cmd, true};
  commands_["SISMEMBER"] = {&sismember_cmd, false};
  commands_["ZADD"] = {&zadd_cmd, true};
  commands_["ZSCORE"] = {&zscore_cmd, false};
  commands_["DEL"] = {&del_cmd, true};
  commands_["EXPIRE"] = {&expire_cmd, true};
  commands_["TTL"] = {&ttl_cmd, false};
}

bool CommandDispatcher::IsWriteCommand(std::string_view command) const {
  std::string cmd_name = common::ToUpperAscii(command);
  auto it = commands_.find(cmd_name);
  return it != commands_.end() && it->second.write_cmd;
}

protocol::Response CommandDispatcher::Execute(
    const std::vector<std::string>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecuteWithResult(args, engine, now_us).response;
}

protocol::Response CommandDispatcher::Execute(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecuteWithResult(args, engine, now_us).response;
}

CommandResult CommandDispatcher::ExecuteWithResult(
    const std::vector<std::string>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecuteWithResult(args, engine, now_us, {});
}

CommandResult CommandDispatcher::ExecuteWithResult(
    const std::vector<std::string>& args, cache::CacheEngine& engine,
    std::uint64_t now_us,
    const CommandReplayOptions& replay_options) const {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const std::string& arg : args) {
    views.push_back(arg);
  }
  return ExecuteWithResult(views, engine, now_us, replay_options);
}

CommandResult CommandDispatcher::ExecuteWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecuteWithResult(args, engine, now_us, {});
}

CommandResult CommandDispatcher::ExecuteWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us,
    const CommandReplayOptions& replay_options) const {
  if (args.empty()) {
    return CommandResult{protocol::Response::Error("ERR empty command"), false};
  }

  std::string cmd_name = common::ToUpperAscii(args[0]);
  auto it = commands_.find(cmd_name);
  if (it == commands_.end()) {
    return CommandResult{
        protocol::Response::Error("ERR unknown command '" + cmd_name + "'"),
        false};
  }

  auto arity_error = it->second.cmd->CheckArity(args, replay_options);
  if (arity_error) {
    return CommandResult{protocol::Response::Error(*arity_error), false};
  }

  return it->second.cmd->ExecWithResult(args, engine, now_us, replay_options);
}

}  // namespace command
