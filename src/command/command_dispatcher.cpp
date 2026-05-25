#include "command/command_dispatcher.h"

#include "command/hash_cmd.h"
#include "command/key_cmd.h"
#include "command/scan_cmd.h"
#include "command/set_cmd.h"
#include "command/string_cmd.h"
#include "command/zset_cmd.h"
#include "common/parse_utils.h"

namespace command {

CommandDispatcher::CommandDispatcher() {
  static SetCmd set_cmd;
  static GetCmd get_cmd;
  static MGetCmd mget_cmd;
  static SetNxCmd setnx_cmd;
  static GetSetCmd getset_cmd;
  static StrLenCmd strlen_cmd;
  static AppendCmd append_cmd;
  static IncrCmd incr_cmd;
  static DecrCmd decr_cmd;
  static IncrByCmd incrby_cmd;
  static DecrByCmd decrby_cmd;
  static HSetCmd hset_cmd;
  static HGetCmd hget_cmd;
  static HDelCmd hdel_cmd;
  static HExistsCmd hexists_cmd;
  static HLenCmd hlen_cmd;
  static HStrLenCmd hstrlen_cmd;
  static HMGetCmd hmget_cmd;
  static HMSetCmd hmset_cmd;
  static HGetAllCmd hgetall_cmd;
  static HKeysCmd hkeys_cmd;
  static HValsCmd hvals_cmd;
  static HIncrByCmd hincrby_cmd;
  static SAddCmd sadd_cmd;
  static SIsMemberCmd sismember_cmd;
  static SRemCmd srem_cmd;
  static SCardCmd scard_cmd;
  static SMembersCmd smembers_cmd;
  static SMIsMemberCmd smismember_cmd;
  static SPopCmd spop_cmd;
  static SRandMemberCmd srandmember_cmd;
  static ZAddCmd zadd_cmd;
  static ZScoreCmd zscore_cmd;
  static ZRemCmd zrem_cmd;
  static ZCardCmd zcard_cmd;
  static ZRankCmd zrank_cmd;
  static ZRevRankCmd zrevrank_cmd;
  static ZCountCmd zcount_cmd;
  static ZIncrByCmd zincrby_cmd;
  static ZRangeCmd zrange_cmd;
  static ExistsCmd exists_cmd;
  static TypeCmd type_cmd;
  static PTtlCmd pttl_cmd;
  static ScanCmd scan_cmd;
  static DelCmd del_cmd;
  static ExpireCmd expire_cmd;
  static TtlCmd ttl_cmd;

  commands_["SET"] = {&set_cmd, true};
  commands_["GET"] = {&get_cmd, false};
  commands_["MGET"] = {&mget_cmd, false};
  commands_["SETNX"] = {&setnx_cmd, true};
  commands_["GETSET"] = {&getset_cmd, true};
  commands_["STRLEN"] = {&strlen_cmd, false};
  commands_["APPEND"] = {&append_cmd, true};
  commands_["INCR"] = {&incr_cmd, true};
  commands_["DECR"] = {&decr_cmd, true};
  commands_["INCRBY"] = {&incrby_cmd, true};
  commands_["DECRBY"] = {&decrby_cmd, true};
  commands_["HSET"] = {&hset_cmd, true};
  commands_["HGET"] = {&hget_cmd, false};
  commands_["HDEL"] = {&hdel_cmd, true};
  commands_["HEXISTS"] = {&hexists_cmd, false};
  commands_["HLEN"] = {&hlen_cmd, false};
  commands_["HSTRLEN"] = {&hstrlen_cmd, false};
  commands_["HMGET"] = {&hmget_cmd, false};
  commands_["HMSET"] = {&hmset_cmd, true};
  commands_["HGETALL"] = {&hgetall_cmd, false};
  commands_["HKEYS"] = {&hkeys_cmd, false};
  commands_["HVALS"] = {&hvals_cmd, false};
  commands_["HINCRBY"] = {&hincrby_cmd, true};
  commands_["SADD"] = {&sadd_cmd, true};
  commands_["SISMEMBER"] = {&sismember_cmd, false};
  commands_["SREM"] = {&srem_cmd, true};
  commands_["SCARD"] = {&scard_cmd, false};
  commands_["SMEMBERS"] = {&smembers_cmd, false};
  commands_["SMISMEMBER"] = {&smismember_cmd, false};
  commands_["SPOP"] = {&spop_cmd, true};
  commands_["SRANDMEMBER"] = {&srandmember_cmd, false};
  commands_["ZADD"] = {&zadd_cmd, true};
  commands_["ZSCORE"] = {&zscore_cmd, false};
  commands_["ZREM"] = {&zrem_cmd, true};
  commands_["ZCARD"] = {&zcard_cmd, false};
  commands_["ZRANK"] = {&zrank_cmd, false};
  commands_["ZREVRANK"] = {&zrevrank_cmd, false};
  commands_["ZCOUNT"] = {&zcount_cmd, false};
  commands_["ZINCRBY"] = {&zincrby_cmd, true};
  commands_["ZRANGE"] = {&zrange_cmd, false};
  commands_["EXISTS"] = {&exists_cmd, false};
  commands_["TYPE"] = {&type_cmd, false};
  commands_["PTTL"] = {&pttl_cmd, false};
  commands_["SCAN"] = {&scan_cmd, false};
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
    return CommandResult{protocol::Response::Error("ERR empty command")};
  }

  std::string cmd_name = common::ToUpperAscii(args[0]);
  auto it = commands_.find(cmd_name);
  if (it == commands_.end()) {
    return CommandResult{
        protocol::Response::Error("ERR unknown command '" + cmd_name + "'")};
  }

  auto arity_error = it->second.cmd->CheckArity(args, replay_options);
  if (arity_error) {
    return CommandResult{protocol::Response::Error(*arity_error)};
  }

  return it->second.cmd->ExecWithResult(args, engine, now_us, replay_options);
}

}  // namespace command
