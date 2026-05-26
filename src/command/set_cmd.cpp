#include "command/set_cmd.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "cache/binlog.h"
#include "cache/redis_object.h"
#include "common/parse_utils.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr const char* kIntegerError =
    "ERR value is not an integer or out of range";
constexpr const char* kOutOfRangeError = "ERR value is out of range";

cache::BinlogRecord MakeRecord(
    std::string_view command, const std::vector<std::string_view>& args) {
  cache::BinlogRecord record;
  record.args.reserve(args.size());
  record.args.push_back(common::ToUpperAscii(command));
  for (std::size_t i = 1; i < args.size(); ++i) {
    record.args.emplace_back(args[i]);
  }
  return record;
}

std::vector<protocol::Response> MembersToResponses(
    const std::vector<cache::PackedString>& members) {
  std::vector<protocol::Response> values;
  values.reserve(members.size());
  for (const cache::PackedString& member : members) {
    values.push_back(protocol::Response::BulkString(member.ToString()));
  }
  return values;
}

std::mt19937_64& RandomEngine() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  return engine;
}

std::vector<cache::PackedString> PickUniqueMembers(
    std::vector<cache::PackedString> members, std::size_t count) {
  std::shuffle(members.begin(), members.end(), RandomEngine());
  if (count < members.size()) {
    members.resize(count);
  }
  return members;
}

std::vector<cache::PackedString> PickMembersWithReplacement(
    const std::vector<cache::PackedString>& members, std::size_t count) {
  std::vector<cache::PackedString> picked;
  if (members.empty() || count == 0) {
    return picked;
  }

  picked.reserve(count);
  std::uniform_int_distribution<std::size_t> dist(0, members.size() - 1);
  for (std::size_t i = 0; i < count; ++i) {
    picked.push_back(members[dist(RandomEngine())]);
  }
  return picked;
}

protocol::Response ReadSet(
    cache::CacheEngine& engine, std::string_view key, std::uint64_t now_us,
    const std::function<protocol::Response(const cache::SetValue*)>& reader) {
  auto obj = engine.Get(key, now_us);
  if (!obj) {
    return reader(nullptr);
  }
  if (obj->Type() != cache::RedisObjectType::kSet) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const cache::SetValue* set = obj->Set();
  if (set == nullptr) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return reader(set);
}

}  // namespace

std::optional<std::string> SAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 3) {
    return "ERR wrong number of arguments for 'sadd' command";
  }
  return std::nullopt;
}

protocol::Response SAddCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SAddCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::string_view key = args[1];
  std::int64_t added_count = 0;
  bool wrong_type = false;
  cache::BinlogRecord record = MakeRecord("SADD", args);
  record.op = cache::BinlogOp::kSAdd;

  engine.Update(
      key,
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        cache::SetValue set;
        std::uint64_t deadline_us = 0;
        if (existing) {
          const cache::SetValue* existing_set = existing->Set();
          if (existing->Type() != cache::RedisObjectType::kSet ||
              existing_set == nullptr) {
            wrong_type = true;
            return std::nullopt;
          }
          set = *existing_set;
          deadline_us = existing->DeadlineUs();
        }
        cache::SetValue next = set;
        for (std::size_t i = 2; i < args.size(); ++i) {
          cache::PackedString member(args[i]);
          if (!next.Contains(member)) {
            next = next.Add(std::move(member));
            ++added_count;
          }
        }
        if (added_count == 0) {
          return std::nullopt;
        }
        cache::RedisObject obj = cache::RedisObject::MakeSet(std::move(next));
        return deadline_us ? obj.WithDeadline(deadline_us) : obj;
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(added_count)};
}

std::optional<std::string> SIsMemberCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'sismember' command";
  }
  return std::nullopt;
}

protocol::Response SIsMemberCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ReadSet(engine, args[1], now_us, [&](const cache::SetValue* set) {
    bool is_member =
        set != nullptr && set->Contains(cache::PackedString(args[2]));
    return protocol::Response::Integer(is_member ? 1 : 0);
  });
}

std::optional<std::string> SRemCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 3) {
    return "ERR wrong number of arguments for 'srem' command";
  }
  return std::nullopt;
}

protocol::Response SRemCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SRemCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t removed_count = 0;
  bool wrong_type = false;
  cache::BinlogRecord record = MakeRecord("SREM", args);

  engine.Mutate(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> cache::MutationResult {
        if (!existing) {
          return cache::MutationResult{false, std::nullopt};
        }
        const cache::SetValue* existing_set = existing->Set();
        if (existing->Type() != cache::RedisObjectType::kSet ||
            existing_set == nullptr) {
          wrong_type = true;
          return cache::MutationResult{false, std::nullopt};
        }

        cache::SetValue next = *existing_set;
        for (std::size_t i = 2; i < args.size(); ++i) {
          cache::PackedString member(args[i]);
          if (next.Contains(member)) {
            auto erased = next.Erase(member);
            if (erased.has_value()) {
              next = std::move(*erased);
              ++removed_count;
            }
          }
        }
        if (removed_count == 0) {
          return cache::MutationResult{false, std::nullopt};
        }
        if (next.Empty()) {
          return cache::MutationResult{true, std::nullopt};
        }
        cache::RedisObject obj = cache::RedisObject::MakeSet(std::move(next));
        return cache::MutationResult{
            true,
            existing->DeadlineUs() ? obj.WithDeadline(existing->DeadlineUs())
                                   : obj};
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(removed_count)};
}

std::optional<std::string> SCardCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'scard' command";
  }
  return std::nullopt;
}

protocol::Response SCardCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ReadSet(engine, args[1], now_us, [](const cache::SetValue* set) {
    return protocol::Response::Integer(
        set == nullptr ? 0 : static_cast<std::int64_t>(set->Size()));
  });
}

std::optional<std::string> SMembersCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'smembers' command";
  }
  return std::nullopt;
}

protocol::Response SMembersCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ReadSet(engine, args[1], now_us, [](const cache::SetValue* set) {
    if (set == nullptr) {
      return protocol::Response::Array({});
    }
    return protocol::Response::Array(MembersToResponses(set->ToVector()));
  });
}

std::optional<std::string> SMIsMemberCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 3) {
    return "ERR wrong number of arguments for 'smismember' command";
  }
  return std::nullopt;
}

protocol::Response SMIsMemberCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ReadSet(engine, args[1], now_us, [&](const cache::SetValue* set) {
    std::vector<protocol::Response> flags;
    flags.reserve(args.size() - 2);
    for (std::size_t i = 2; i < args.size(); ++i) {
      bool is_member =
          set != nullptr && set->Contains(cache::PackedString(args[i]));
      flags.push_back(protocol::Response::Integer(is_member ? 1 : 0));
    }
    return protocol::Response::Array(std::move(flags));
  });
}

std::optional<std::string> SPopCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2 && args.size() != 3) {
    return "ERR wrong number of arguments for 'spop' command";
  }
  return std::nullopt;
}

protocol::Response SPopCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SPopCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  const bool has_count = args.size() == 3;
  std::int64_t parsed_count = 1;
  if (has_count) {
    if (!common::ParseInt64(args[2], &parsed_count) || parsed_count < 0) {
      return CommandResult{protocol::Response::Error(kIntegerError)};
    }
    if (parsed_count == 0) {
      return CommandResult{ReadSet(
          engine, args[1], now_us, [](const cache::SetValue* /*set*/) {
            return protocol::Response::Array({});
          })};
    }
  }

  bool wrong_type = false;
  std::vector<cache::PackedString> picked;
  cache::BinlogRecord record = MakeRecord("SPOP", args);

  engine.Mutate(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> cache::MutationResult {
        if (!existing) {
          return cache::MutationResult{false, std::nullopt};
        }
        const cache::SetValue* existing_set = existing->Set();
        if (existing->Type() != cache::RedisObjectType::kSet ||
            existing_set == nullptr) {
          wrong_type = true;
          return cache::MutationResult{false, std::nullopt};
        }

        std::vector<cache::PackedString> members = existing_set->ToVector();
        if (members.empty()) {
          return cache::MutationResult{false, std::nullopt};
        }

        const std::size_t requested =
            has_count ? static_cast<std::size_t>(parsed_count) : std::size_t{1};
        picked = PickUniqueMembers(std::move(members), requested);

        cache::SetValue next = *existing_set;
        for (const cache::PackedString& member : picked) {
          auto erased = next.Erase(member);
          if (erased.has_value()) {
            next = std::move(*erased);
          }
        }
        if (next.Empty()) {
          return cache::MutationResult{true, std::nullopt};
        }
        cache::RedisObject obj = cache::RedisObject::MakeSet(std::move(next));
        return cache::MutationResult{
            true,
            existing->DeadlineUs() ? obj.WithDeadline(existing->DeadlineUs())
                                   : obj};
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (has_count) {
    return CommandResult{
        protocol::Response::Array(MembersToResponses(picked))};
  }
  if (picked.empty()) {
    return CommandResult{protocol::Response::NullBulk()};
  }
  return CommandResult{
      protocol::Response::BulkString(picked.front().ToString())};
}

std::optional<std::string> SRandMemberCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2 && args.size() != 3) {
    return "ERR wrong number of arguments for 'srandmember' command";
  }
  return std::nullopt;
}

protocol::Response SRandMemberCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  const bool has_count = args.size() == 3;
  std::int64_t parsed_count = 1;
  if (has_count && !common::ParseInt64(args[2], &parsed_count)) {
    return protocol::Response::Error(kIntegerError);
  }
  if (has_count && parsed_count == std::numeric_limits<std::int64_t>::min()) {
    return protocol::Response::Error(kOutOfRangeError);
  }

  return ReadSet(engine, args[1], now_us, [&](const cache::SetValue* set) {
    if (set == nullptr || set->Empty()) {
      return has_count ? protocol::Response::Array({})
                       : protocol::Response::NullBulk();
    }

    std::vector<cache::PackedString> members = set->ToVector();
    if (!has_count) {
      std::vector<cache::PackedString> picked =
          PickMembersWithReplacement(members, 1);
      return protocol::Response::BulkString(picked.front().ToString());
    }
    if (parsed_count == 0) {
      return protocol::Response::Array({});
    }
    if (parsed_count > 0) {
      return protocol::Response::Array(MembersToResponses(PickUniqueMembers(
          std::move(members), static_cast<std::size_t>(parsed_count))));
    }

    std::size_t count = static_cast<std::size_t>(-parsed_count);
    return protocol::Response::Array(
        MembersToResponses(PickMembersWithReplacement(members, count)));
  });
}

}  // namespace command
