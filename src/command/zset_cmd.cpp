#include "command/zset_cmd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cache/binlog.h"
#include "cache/redis_object.h"
#include "common/parse_utils.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr const char* kSyntaxError = "ERR syntax error";
constexpr const char* kInvalidFloatError = "ERR value is not a valid float";
constexpr const char* kInvalidIncrementError =
    "ERR increment would produce NaN or Infinity";

struct ZEntry {
  std::string member;
  double score = 0.0;
};

struct ScoreBound {
  double value = 0.0;
  bool inclusive = true;
};

struct LexBound {
  enum class Kind { kNegInf, kPosInf, kValue };

  Kind kind = Kind::kValue;
  std::string value;
  bool inclusive = true;
};

struct ZAddOptions {
  bool nx = false;
  bool xx = false;
  bool gt = false;
  bool lt = false;
  bool ch = false;
  bool incr = false;
};

struct ZAddInput {
  double score = 0.0;
  std::string_view member;
};

enum class ZAddParseStatus { kOk, kSyntax, kInvalidFloat };
enum class RangeMode { kRank, kScore, kLex };

struct ZRangeOptions {
  RangeMode mode = RangeMode::kRank;
  bool rev = false;
  bool with_scores = false;
  bool has_limit = false;
  std::int64_t offset = 0;
  std::int64_t count = 0;
};

double NormalizeZero(double value) {
  if (value == 0.0) {
    return 0.0;
  }
  return value;
}

std::string FormatScore(double score) {
  score = NormalizeZero(score);
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

cache::BinlogRecord MakeRecord(
    const std::vector<std::string_view>& args,
    std::optional<cache::BinlogOp> op = std::nullopt) {
  cache::BinlogRecord record;
  record.op = op;
  record.args.reserve(args.size());
  if (!args.empty()) {
    record.args.push_back(common::ToUpperAscii(args[0]));
  }
  for (std::size_t i = 1; i < args.size(); ++i) {
    record.args.emplace_back(args[i]);
  }
  return record;
}

bool ParseFiniteScore(std::string_view text, double* out) {
  double score = 0.0;
  if (!common::ParseFiniteDouble(text, &score)) {
    return false;
  }
  *out = NormalizeZero(score);
  return true;
}

bool ParseScoreValue(std::string_view text, bool allow_infinity, double* out) {
  const std::string upper = common::ToUpperAscii(text);
  if (allow_infinity &&
      (upper == "+INF" || upper == "INF" || upper == "+INFINITY" ||
       upper == "INFINITY")) {
    *out = std::numeric_limits<double>::infinity();
    return true;
  }
  if (allow_infinity && (upper == "-INF" || upper == "-INFINITY")) {
    *out = -std::numeric_limits<double>::infinity();
    return true;
  }
  return ParseFiniteScore(text, out);
}

bool ParseScoreBound(std::string_view text, ScoreBound* bound) {
  bool inclusive = true;
  if (!text.empty() && text.front() == '(') {
    inclusive = false;
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return false;
  }

  double value = 0.0;
  if (!ParseScoreValue(text, true, &value)) {
    return false;
  }
  *bound = ScoreBound{value, inclusive};
  return true;
}

bool ScoreInRange(double score, const ScoreBound& lower,
                  const ScoreBound& upper) {
  const bool above_lower =
      lower.inclusive ? score >= lower.value : score > lower.value;
  const bool below_upper =
      upper.inclusive ? score <= upper.value : score < upper.value;
  return above_lower && below_upper;
}

int CompareBinaryLex(std::string_view left, std::string_view right) {
  const std::size_t shared_size = std::min(left.size(), right.size());
  if (shared_size != 0) {
    const int compared = std::memcmp(left.data(), right.data(), shared_size);
    if (compared < 0) {
      return -1;
    }
    if (compared > 0) {
      return 1;
    }
  }
  if (left.size() < right.size()) {
    return -1;
  }
  if (left.size() > right.size()) {
    return 1;
  }
  return 0;
}

bool ParseLexBound(std::string_view text, LexBound* bound) {
  if (text == "-") {
    *bound = LexBound{LexBound::Kind::kNegInf, "", true};
    return true;
  }
  if (text == "+") {
    *bound = LexBound{LexBound::Kind::kPosInf, "", true};
    return true;
  }
  if (!text.empty() && (text.front() == '[' || text.front() == '(')) {
    const bool inclusive = text.front() == '[';
    text.remove_prefix(1);
    *bound = LexBound{LexBound::Kind::kValue, std::string(text), inclusive};
    return true;
  }
  return false;
}

bool LexAboveLower(std::string_view member, const LexBound& lower) {
  if (lower.kind == LexBound::Kind::kNegInf) {
    return true;
  }
  if (lower.kind == LexBound::Kind::kPosInf) {
    return false;
  }
  const int compared = CompareBinaryLex(member, lower.value);
  return lower.inclusive ? compared >= 0 : compared > 0;
}

bool LexBelowUpper(std::string_view member, const LexBound& upper) {
  if (upper.kind == LexBound::Kind::kPosInf) {
    return true;
  }
  if (upper.kind == LexBound::Kind::kNegInf) {
    return false;
  }
  const int compared = CompareBinaryLex(member, upper.value);
  return upper.inclusive ? compared <= 0 : compared < 0;
}

bool LexInRange(std::string_view member, const LexBound& lower,
                const LexBound& upper) {
  return LexAboveLower(member, lower) && LexBelowUpper(member, upper);
}

cache::RedisObject PreserveDeadline(cache::RedisObject object,
                                    std::uint64_t deadline_us) {
  if (deadline_us == 0) {
    return object;
  }
  return object.WithDeadline(deadline_us);
}

std::vector<ZEntry> EntriesByLex(const cache::ZSetValue& zset) {
  std::vector<ZEntry> entries;
  entries.reserve(zset.Size());
  zset.ForEach([&](const cache::PackedString& member, double score) {
    entries.push_back(ZEntry{member.ToString(), score});
  });
  std::sort(entries.begin(), entries.end(),
            [](const ZEntry& left, const ZEntry& right) {
              return CompareBinaryLex(left.member, right.member) < 0;
            });
  return entries;
}

std::vector<ZEntry> EntriesByScore(const cache::ZSetValue& zset) {
  std::vector<ZEntry> entries = EntriesByLex(zset);
  std::sort(entries.begin(), entries.end(),
            [](const ZEntry& left, const ZEntry& right) {
              if (left.score != right.score) {
                return left.score < right.score;
              }
              return CompareBinaryLex(left.member, right.member) < 0;
            });
  return entries;
}

std::vector<ZEntry> EntriesByScoreReverse(const cache::ZSetValue& zset) {
  std::vector<ZEntry> entries = EntriesByScore(zset);
  std::reverse(entries.begin(), entries.end());
  return entries;
}

protocol::Response AppendRangeResponse(const std::vector<ZEntry>& entries,
                                        bool with_scores) {
  std::vector<protocol::Response> elements;
  elements.reserve(entries.size() * (with_scores ? 2 : 1));
  for (const ZEntry& entry : entries) {
    elements.push_back(protocol::Response::BulkString(entry.member));
    if (with_scores) {
      elements.push_back(
          protocol::Response::BulkString(FormatScore(entry.score)));
    }
  }
  return protocol::Response::Array(std::move(elements));
}

std::vector<ZEntry> FilterByScore(const std::vector<ZEntry>& entries,
                                  const ScoreBound& lower,
                                  const ScoreBound& upper) {
  std::vector<ZEntry> result;
  result.reserve(entries.size());
  for (const ZEntry& entry : entries) {
    if (ScoreInRange(entry.score, lower, upper)) {
      result.push_back(entry);
    }
  }
  return result;
}

std::vector<ZEntry> FilterByLex(const std::vector<ZEntry>& entries,
                                const LexBound& lower,
                                const LexBound& upper) {
  std::vector<ZEntry> result;
  result.reserve(entries.size());
  for (const ZEntry& entry : entries) {
    if (LexInRange(entry.member, lower, upper)) {
      result.push_back(entry);
    }
  }
  return result;
}

std::vector<ZEntry> ApplyRankRange(std::vector<ZEntry> entries,
                                   std::int64_t start, std::int64_t stop) {
  const std::int64_t size = static_cast<std::int64_t>(entries.size());
  if (size == 0) {
    return {};
  }
  if (start < 0) {
    start += size;
  }
  if (stop < 0) {
    stop += size;
  }
  if (start < 0) {
    start = 0;
  }
  if (stop < 0 || start >= size) {
    return {};
  }
  if (stop >= size) {
    stop = size - 1;
  }
  if (start > stop) {
    return {};
  }

  return std::vector<ZEntry>(entries.begin() + start,
                             entries.begin() + stop + 1);
}

std::vector<ZEntry> ApplyLimit(std::vector<ZEntry> entries,
                               std::int64_t offset, std::int64_t count) {
  if (offset < 0 || count == 0) {
    return {};
  }
  const std::int64_t size = static_cast<std::int64_t>(entries.size());
  if (offset >= size) {
    return {};
  }
  std::int64_t end = size;
  if (count > 0 && count < size - offset) {
    end = offset + count;
  }
  if (end <= offset) {
    return {};
  }
  return std::vector<ZEntry>(entries.begin() + offset, entries.begin() + end);
}

struct ZSetRead {
  enum class Status { kMissing, kWrongType, kOk };

  Status status = Status::kMissing;
  cache::ZSetValue zset;
};

ZSetRead ReadZSet(cache::CacheEngine& engine, std::string_view key,
                  std::uint64_t now_us) {
  auto obj = engine.Get(key, now_us);
  if (!obj) {
    return ZSetRead{ZSetRead::Status::kMissing, {}};
  }
  if (obj->Type() != cache::RedisObjectType::kZSet || obj->ZSet() == nullptr) {
    return ZSetRead{ZSetRead::Status::kWrongType, {}};
  }
  return ZSetRead{ZSetRead::Status::kOk, *obj->ZSet()};
}

ZAddParseStatus ParseZAddArgs(const std::vector<std::string_view>& args,
                              ZAddOptions* options,
                              std::vector<ZAddInput>* inputs) {
  std::size_t index = 2;
  for (; index < args.size(); ++index) {
    const std::string option = common::ToUpperAscii(args[index]);
    if (option == "NX") {
      if (options->nx) {
        return ZAddParseStatus::kSyntax;
      }
      options->nx = true;
    } else if (option == "XX") {
      if (options->xx) {
        return ZAddParseStatus::kSyntax;
      }
      options->xx = true;
    } else if (option == "GT") {
      if (options->gt) {
        return ZAddParseStatus::kSyntax;
      }
      options->gt = true;
    } else if (option == "LT") {
      if (options->lt) {
        return ZAddParseStatus::kSyntax;
      }
      options->lt = true;
    } else if (option == "CH") {
      if (options->ch) {
        return ZAddParseStatus::kSyntax;
      }
      options->ch = true;
    } else if (option == "INCR") {
      if (options->incr) {
        return ZAddParseStatus::kSyntax;
      }
      options->incr = true;
    } else {
      break;
    }
  }

  if ((options->nx && options->xx) || (options->nx && options->gt) ||
      (options->nx && options->lt) || (options->gt && options->lt)) {
    return ZAddParseStatus::kSyntax;
  }
  const std::size_t remaining = args.size() - index;
  if (remaining < 2 || remaining % 2 != 0) {
    return ZAddParseStatus::kSyntax;
  }
  if (options->incr && remaining != 2) {
    return ZAddParseStatus::kSyntax;
  }

  inputs->clear();
  inputs->reserve(remaining / 2);
  for (; index < args.size(); index += 2) {
    double score = 0.0;
    if (!ParseFiniteScore(args[index], &score)) {
      return ZAddParseStatus::kInvalidFloat;
    }
    inputs->push_back(ZAddInput{score, args[index + 1]});
  }
  return ZAddParseStatus::kOk;
}

CommandResult IncrementZSetMember(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us, std::string_view key, double increment,
    std::string_view member, const ZAddOptions& options,
    std::optional<cache::BinlogOp> op) {
  bool wrong_type = false;
  bool prevented = false;
  bool invalid_result = false;
  bool result_ready = false;
  double result_score = 0.0;
  cache::BinlogRecord record = MakeRecord(args, op);

  engine.Mutate(
      key,
      [&](std::optional<cache::RedisObject> existing)
          -> cache::MutationResult {
        cache::ZSetValue zset;
        std::uint64_t deadline_us = 0;
        if (existing) {
          const cache::ZSetValue* existing_zset = existing->ZSet();
          if (existing->Type() != cache::RedisObjectType::kZSet ||
              existing_zset == nullptr) {
            wrong_type = true;
            return cache::MutationResult{false, std::nullopt};
          }
          zset = *existing_zset;
          deadline_us = existing->DeadlineUs();
        }

        const cache::PackedString packed_member(member);
        const double* found = zset.Find(packed_member);
        const bool exists = found != nullptr;
        if ((exists && options.nx) || (!exists && options.xx)) {
          prevented = true;
          return cache::MutationResult{false, std::nullopt};
        }

        const double old_score = exists ? *found : 0.0;
        const double next_score = NormalizeZero(old_score + increment);
        if (!std::isfinite(next_score)) {
          invalid_result = true;
          return cache::MutationResult{false, std::nullopt};
        }
        if (exists && options.gt && !(next_score > old_score)) {
          prevented = true;
          return cache::MutationResult{false, std::nullopt};
        }
        if (exists && options.lt && !(next_score < old_score)) {
          prevented = true;
          return cache::MutationResult{false, std::nullopt};
        }

        result_ready = true;
        result_score = next_score;
        if (exists && next_score == old_score) {
          return cache::MutationResult{false, std::nullopt};
        }

        cache::RedisObject obj =
            cache::RedisObject::MakeZSet(zset.Set(packed_member, next_score));
        return cache::MutationResult{
            true, PreserveDeadline(std::move(obj), deadline_us)};
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (invalid_result) {
    return CommandResult{protocol::Response::Error(kInvalidIncrementError)};
  }
  if (prevented && !result_ready) {
    return CommandResult{protocol::Response::NullBulk()};
  }
  return CommandResult{
      protocol::Response::BulkString(FormatScore(result_score))};
}

std::optional<protocol::Response> ParseZRangeOptions(
    const std::vector<std::string_view>& args, ZRangeOptions* options) {
  for (std::size_t index = 4; index < args.size(); ++index) {
    const std::string option = common::ToUpperAscii(args[index]);
    if (option == "BYSCORE") {
      if (options->mode != RangeMode::kRank) {
        return protocol::Response::Error(kSyntaxError);
      }
      options->mode = RangeMode::kScore;
    } else if (option == "BYLEX") {
      if (options->mode != RangeMode::kRank) {
        return protocol::Response::Error(kSyntaxError);
      }
      options->mode = RangeMode::kLex;
    } else if (option == "REV") {
      if (options->rev) {
        return protocol::Response::Error(kSyntaxError);
      }
      options->rev = true;
    } else if (option == "WITHSCORES") {
      if (options->with_scores) {
        return protocol::Response::Error(kSyntaxError);
      }
      options->with_scores = true;
    } else if (option == "LIMIT") {
      if (options->has_limit || index + 2 >= args.size()) {
        return protocol::Response::Error(kSyntaxError);
      }
      if (!common::ParseInt64(args[index + 1], &options->offset) ||
          !common::ParseInt64(args[index + 2], &options->count)) {
        return protocol::Response::Error(kSyntaxError);
      }
      options->has_limit = true;
      index += 2;
    } else {
      return protocol::Response::Error(kSyntaxError);
    }
  }
  if (options->has_limit && options->mode == RangeMode::kRank) {
    return protocol::Response::Error(kSyntaxError);
  }
  return std::nullopt;
}

protocol::Response RankResponse(cache::CacheEngine& engine,
                                std::uint64_t now_us, std::string_view key,
                                std::string_view member, bool reverse) {
  ZSetRead read = ReadZSet(engine, key, now_us);
  if (read.status == ZSetRead::Status::kMissing) {
    return protocol::Response::NullBulk();
  }
  if (read.status == ZSetRead::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::vector<ZEntry> entries =
      reverse ? EntriesByScoreReverse(read.zset) : EntriesByScore(read.zset);
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].member == member) {
      return protocol::Response::Integer(static_cast<std::int64_t>(index));
    }
  }
  return protocol::Response::NullBulk();
}

}  // namespace

std::optional<std::string> ZAddCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 4) {
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
  ZAddOptions options;
  std::vector<ZAddInput> inputs;
  const ZAddParseStatus parse_status = ParseZAddArgs(args, &options, &inputs);
  if (parse_status == ZAddParseStatus::kSyntax) {
    return CommandResult{protocol::Response::Error(kSyntaxError)};
  }
  if (parse_status == ZAddParseStatus::kInvalidFloat) {
    return CommandResult{protocol::Response::Error(kInvalidFloatError)};
  }

  if (options.incr) {
    return IncrementZSetMember(args, engine, now_us, args[1], inputs[0].score,
                               inputs[0].member, options,
                               cache::BinlogOp::kZAdd);
  }

  std::int64_t added_count = 0;
  std::int64_t changed_count = 0;
  bool wrong_type = false;
  cache::BinlogRecord record = MakeRecord(args, cache::BinlogOp::kZAdd);

  engine.Mutate(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> cache::MutationResult {
        cache::ZSetValue zset;
        std::uint64_t deadline_us = 0;
        if (existing) {
          const cache::ZSetValue* existing_zset = existing->ZSet();
          if (existing->Type() != cache::RedisObjectType::kZSet ||
              existing_zset == nullptr) {
            wrong_type = true;
            return cache::MutationResult{false, std::nullopt};
          }
          zset = *existing_zset;
          deadline_us = existing->DeadlineUs();
        }

        bool changed = false;
        for (const ZAddInput& input : inputs) {
          const cache::PackedString member(input.member);
          const double* found = zset.Find(member);
          if (found == nullptr) {
            if (options.xx) {
              continue;
            }
            zset = zset.Set(member, input.score);
            ++added_count;
            changed = true;
            continue;
          }

          if (options.nx) {
            continue;
          }
          if (options.gt && !(input.score > *found)) {
            continue;
          }
          if (options.lt && !(input.score < *found)) {
            continue;
          }
          if (input.score == *found) {
            continue;
          }
          zset = zset.Set(member, input.score);
          ++changed_count;
          changed = true;
        }

        if (!changed) {
          return cache::MutationResult{false, std::nullopt};
        }
        cache::RedisObject obj = cache::RedisObject::MakeZSet(std::move(zset));
        return cache::MutationResult{
            true, PreserveDeadline(std::move(obj), deadline_us)};
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(
      options.ch ? added_count + changed_count : added_count)};
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

std::optional<std::string> ZRemCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 3) {
    return "ERR wrong number of arguments for 'zrem' command";
  }
  return std::nullopt;
}

protocol::Response ZRemCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult ZRemCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t removed_count = 0;
  bool wrong_type = false;
  cache::BinlogRecord record = MakeRecord(args);

  engine.Mutate(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> cache::MutationResult {
        if (!existing) {
          return cache::MutationResult{false, std::nullopt};
        }
        const cache::ZSetValue* existing_zset = existing->ZSet();
        if (existing->Type() != cache::RedisObjectType::kZSet ||
            existing_zset == nullptr) {
          wrong_type = true;
          return cache::MutationResult{false, std::nullopt};
        }

        cache::ZSetValue zset = *existing_zset;
        for (std::size_t index = 2; index < args.size(); ++index) {
          std::optional<cache::ZSetValue> next =
              zset.Erase(cache::PackedString(args[index]));
          if (!next) {
            continue;
          }
          zset = std::move(*next);
          ++removed_count;
        }
        if (removed_count == 0) {
          return cache::MutationResult{false, std::nullopt};
        }
        if (zset.Empty()) {
          return cache::MutationResult{true, std::nullopt};
        }
        cache::RedisObject obj = cache::RedisObject::MakeZSet(std::move(zset));
        return cache::MutationResult{
            true, PreserveDeadline(std::move(obj), existing->DeadlineUs())};
      },
      std::move(record), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(removed_count)};
}

std::optional<std::string> ZCardCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'zcard' command";
  }
  return std::nullopt;
}

protocol::Response ZCardCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  ZSetRead read = ReadZSet(engine, args[1], now_us);
  if (read.status == ZSetRead::Status::kMissing) {
    return protocol::Response::Integer(0);
  }
  if (read.status == ZSetRead::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(
      static_cast<std::int64_t>(read.zset.Size()));
}

std::optional<std::string> ZRankCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'zrank' command";
  }
  return std::nullopt;
}

protocol::Response ZRankCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return RankResponse(engine, now_us, args[1], args[2], false);
}

std::optional<std::string> ZRevRankCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'zrevrank' command";
  }
  return std::nullopt;
}

protocol::Response ZRevRankCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return RankResponse(engine, now_us, args[1], args[2], true);
}

std::optional<std::string> ZCountCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) {
    return "ERR wrong number of arguments for 'zcount' command";
  }
  return std::nullopt;
}

protocol::Response ZCountCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  ScoreBound lower;
  ScoreBound upper;
  if (!ParseScoreBound(args[2], &lower) || !ParseScoreBound(args[3], &upper)) {
    return protocol::Response::Error(kInvalidFloatError);
  }

  ZSetRead read = ReadZSet(engine, args[1], now_us);
  if (read.status == ZSetRead::Status::kMissing) {
    return protocol::Response::Integer(0);
  }
  if (read.status == ZSetRead::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::int64_t count = 0;
  read.zset.ForEach([&](const cache::PackedString&, double score) {
    if (ScoreInRange(score, lower, upper)) {
      ++count;
    }
  });
  return protocol::Response::Integer(count);
}

std::optional<std::string> ZIncrByCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 4) {
    return "ERR wrong number of arguments for 'zincrby' command";
  }
  return std::nullopt;
}

protocol::Response ZIncrByCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult ZIncrByCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  double increment = 0.0;
  if (!ParseFiniteScore(args[2], &increment)) {
    return CommandResult{protocol::Response::Error(kInvalidFloatError)};
  }
  return IncrementZSetMember(args, engine, now_us, args[1], increment, args[3],
                             ZAddOptions{}, std::nullopt);
}

std::optional<std::string> ZRangeCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 4) {
    return "ERR wrong number of arguments for 'zrange' command";
  }
  return std::nullopt;
}

protocol::Response ZRangeCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  ZRangeOptions options;
  if (std::optional<protocol::Response> error =
          ParseZRangeOptions(args, &options)) {
    return *error;
  }

  std::int64_t rank_start = 0;
  std::int64_t rank_stop = 0;
  ScoreBound score_lower;
  ScoreBound score_upper;
  LexBound lex_lower;
  LexBound lex_upper;
  if (options.mode == RangeMode::kRank) {
    if (!common::ParseInt64(args[2], &rank_start) ||
        !common::ParseInt64(args[3], &rank_stop)) {
      return protocol::Response::Error(kSyntaxError);
    }
  } else if (options.mode == RangeMode::kScore) {
    const std::string_view lower_arg = options.rev ? args[3] : args[2];
    const std::string_view upper_arg = options.rev ? args[2] : args[3];
    if (!ParseScoreBound(lower_arg, &score_lower) ||
        !ParseScoreBound(upper_arg, &score_upper)) {
      return protocol::Response::Error(kInvalidFloatError);
    }
  } else {
    const std::string_view lower_arg = options.rev ? args[3] : args[2];
    const std::string_view upper_arg = options.rev ? args[2] : args[3];
    if (!ParseLexBound(lower_arg, &lex_lower) ||
        !ParseLexBound(upper_arg, &lex_upper)) {
      return protocol::Response::Error(kSyntaxError);
    }
  }

  ZSetRead read = ReadZSet(engine, args[1], now_us);
  if (read.status == ZSetRead::Status::kMissing) {
    return protocol::Response::Array({});
  }
  if (read.status == ZSetRead::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }

  std::vector<ZEntry> entries;
  if (options.mode == RangeMode::kRank) {
    entries = options.rev ? EntriesByScoreReverse(read.zset)
                          : EntriesByScore(read.zset);
    entries = ApplyRankRange(std::move(entries), rank_start, rank_stop);
    return AppendRangeResponse(entries, options.with_scores);
  }

  if (options.mode == RangeMode::kScore) {
    entries = EntriesByScore(read.zset);
    entries = FilterByScore(entries, score_lower, score_upper);
  } else {
    entries = EntriesByLex(read.zset);
    entries = FilterByLex(entries, lex_lower, lex_upper);
  }

  if (options.rev) {
    std::reverse(entries.begin(), entries.end());
  }
  if (options.has_limit) {
    entries = ApplyLimit(std::move(entries), options.offset, options.count);
  }
  return AppendRangeResponse(entries, options.with_scores);
}

}  // namespace command
