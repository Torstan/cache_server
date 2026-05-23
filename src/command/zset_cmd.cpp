#include "command/zset_cmd.h"

#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/redis_object.h"
#include "common/parse_utils.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

std::string FormatScore(double score) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << score;
  std::string text = out.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

}  // namespace

std::optional<std::string> ZAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) {
    return "ERR wrong number of arguments for 'zadd' command";
  }
  return std::nullopt;
}

protocol::Response ZAddCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult ZAddCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  double score = 0.0;
  if (!common::ParseFiniteDouble(args[2], &score)) {
    return CommandResult{
        protocol::Response::Error("ERR value is not a valid float")};
  }

  std::string_view key = args[1], member = args[3];
  bool created = false;
  bool wrong_type = false;
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kZAdd;
  record.args = {"ZADD", std::string(key), FormatScore(score),
                 std::string(member)};

  engine.Update(
      key,
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        cache::ZSetValue zset;
        std::uint64_t deadline_us = 0;
        if (existing) {
          const cache::ZSetValue* existing_zset = existing->ZSet();
          if (existing->Type() != cache::RedisObjectType::kZSet ||
              existing_zset == nullptr) {
            wrong_type = true;
            return std::nullopt;
          }
          zset = *existing_zset;
          deadline_us = existing->DeadlineUs();
          created = zset.Find(cache::PackedString(member)) == nullptr;
        } else {
          created = true;
        }
        cache::ZSetValue next = zset.Set(cache::PackedString(member), score);
        cache::RedisObject obj = cache::RedisObject::MakeZSet(std::move(next));
        return deadline_us ? obj.WithDeadline(deadline_us) : obj;
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(created ? 1 : 0)};
}

std::optional<std::string> ZScoreCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'zscore' command";
  }
  return std::nullopt;
}

protocol::Response ZScoreCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  auto obj = engine.Get(args[1], now_us);
  if (!obj) {
    return protocol::Response::NullBulk();
  }
  if (obj->Type() != cache::RedisObjectType::kZSet) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const cache::ZSetValue* zset = obj->ZSet();
  if (zset == nullptr) {
    return protocol::Response::Error(kWrongTypeError);
  }
  const double* found = zset->Find(cache::PackedString(args[2]));
  if (!found) {
    return protocol::Response::NullBulk();
  }
  return protocol::Response::BulkString(FormatScore(*found));
}

}  // namespace command
