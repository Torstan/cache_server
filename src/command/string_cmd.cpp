#include "command/string_cmd.h"

#include <cstdint>
#include <limits>
#include <optional>
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
constexpr const char* kSyntaxError = "ERR syntax error";
constexpr const char* kInvalidExpireError =
    "ERR invalid expire time in 'set' command";
constexpr const char* kIntegerError =
    "ERR value is not an integer or out of range";
constexpr const char* kOverflowError =
    "ERR increment or decrement would overflow";

constexpr std::uint64_t kMicrosPerSecond = 1'000'000ULL;
constexpr std::uint64_t kMicrosPerMillisecond = 1'000ULL;

enum class StringReadStatus { kMissing, kWrongType, kOk };

struct StringRead {
  StringReadStatus status = StringReadStatus::kMissing;
  std::string value;
  std::uint64_t deadline_us = 0;
};

struct SetOptions {
  bool nx = false;
  bool xx = false;
  bool get = false;
  bool keep_ttl = false;
  std::uint64_t deadline_us = 0;
};

enum class IntegerMutationMode { kAdd, kSubtract };

cache::BinlogRecord MakeRecord(
    std::string_view command, const std::vector<std::string_view>& args,
    std::optional<cache::BinlogOp> op = std::nullopt) {
  cache::BinlogRecord record;
  record.op = op;
  record.args.reserve(args.size());
  record.args.push_back(common::ToUpperAscii(command));
  for (std::size_t i = 1; i < args.size(); ++i) {
    record.args.push_back(std::string(args[i]));
  }
  return record;
}

StringRead ReadStringObject(const std::optional<cache::RedisObject>& object) {
  if (!object.has_value()) {
    return {};
  }
  const cache::PackedString* value = object->StringValue();
  if (object->Type() != cache::RedisObjectType::kString || value == nullptr) {
    return {StringReadStatus::kWrongType, {}, object->DeadlineUs()};
  }
  return {StringReadStatus::kOk, value->ToString(), object->DeadlineUs()};
}

StringRead ReadStringObject(cache::CacheEngine& engine, std::string_view key,
                            std::uint64_t now_us) {
  return ReadStringObject(engine.Get(key, now_us));
}

bool ParseIntegerString(std::string_view text, std::int64_t* out) {
  return common::ParseInt64(text, out);
}

bool CheckedAddInt64(std::int64_t left, std::int64_t right,
                     std::int64_t* out) {
  if (right > 0 &&
      left > std::numeric_limits<std::int64_t>::max() - right) {
    return false;
  }
  if (right < 0 &&
      left < std::numeric_limits<std::int64_t>::min() - right) {
    return false;
  }
  *out = left + right;
  return true;
}

bool CheckedSubtractInt64(std::int64_t left, std::int64_t right,
                          std::int64_t* out) {
  if (right > 0 &&
      left < std::numeric_limits<std::int64_t>::min() + right) {
    return false;
  }
  if (right < 0 &&
      left > std::numeric_limits<std::int64_t>::max() + right) {
    return false;
  }
  *out = left - right;
  return true;
}

std::uint64_t SaturatingMultiply(std::uint64_t value,
                                 std::uint64_t multiplier) {
  if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return value * multiplier;
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

cache::RedisObject ApplyStringDeadline(cache::RedisObject object,
                                       std::uint64_t deadline_us,
                                       bool keep_ttl,
                                       std::uint64_t existing_deadline) {
  if (deadline_us != 0) {
    return object.WithDeadline(deadline_us);
  }
  if (keep_ttl && existing_deadline != 0) {
    return object.WithDeadline(existing_deadline);
  }
  return object.ClearDeadline();
}

std::optional<std::string> ParsePositiveExpiration(std::string_view text,
                                                   std::uint64_t* value) {
  std::int64_t parsed = 0;
  if (!ParseIntegerString(text, &parsed) || parsed <= 0) {
    return std::string(kInvalidExpireError);
  }
  *value = static_cast<std::uint64_t>(parsed);
  return std::nullopt;
}

std::optional<std::string> ParseSetOptions(
    const std::vector<std::string_view>& args, std::uint64_t now_us,
    SetOptions* options) {
  bool has_expiration_option = false;
  for (std::size_t i = 3; i < args.size();) {
    const std::string option = common::ToUpperAscii(args[i]);
    if (option == "NX") {
      if (options->nx || options->xx) {
        return std::string(kSyntaxError);
      }
      options->nx = true;
      ++i;
    } else if (option == "XX") {
      if (options->nx || options->xx) {
        return std::string(kSyntaxError);
      }
      options->xx = true;
      ++i;
    } else if (option == "GET") {
      if (options->get) {
        return std::string(kSyntaxError);
      }
      options->get = true;
      ++i;
    } else if (option == "KEEPTTL") {
      if (has_expiration_option) {
        return std::string(kSyntaxError);
      }
      has_expiration_option = true;
      options->keep_ttl = true;
      ++i;
    } else if (option == "EX" || option == "PX" || option == "EXAT" ||
               option == "PXAT") {
      if (has_expiration_option || i + 1 >= args.size()) {
        return std::string(kSyntaxError);
      }
      std::uint64_t amount = 0;
      if (auto error = ParsePositiveExpiration(args[i + 1], &amount)) {
        return error;
      }
      has_expiration_option = true;
      if (option == "EX") {
        options->deadline_us =
            SaturatingAdd(now_us, SaturatingMultiply(amount, kMicrosPerSecond));
      } else if (option == "PX") {
        options->deadline_us = SaturatingAdd(
            now_us, SaturatingMultiply(amount, kMicrosPerMillisecond));
      } else if (option == "EXAT") {
        options->deadline_us = SaturatingMultiply(amount, kMicrosPerSecond);
      } else {
        options->deadline_us =
            SaturatingMultiply(amount, kMicrosPerMillisecond);
      }
      i += 2;
    } else {
      return std::string(kSyntaxError);
    }
  }
  if (options->nx && options->get) {
    return std::string(kSyntaxError);
  }
  return std::nullopt;
}

CommandResult ExecIntegerMutation(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us, std::int64_t operand,
    IntegerMutationMode mode) {
  bool wrong_type = false;
  bool invalid_integer = false;
  bool overflow = false;
  std::int64_t next_value = 0;

  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        std::int64_t current = 0;
        std::uint64_t deadline_us = 0;
        if (existing.has_value()) {
          StringRead read = ReadStringObject(existing);
          if (read.status == StringReadStatus::kWrongType) {
            wrong_type = true;
            return std::nullopt;
          }
          if (!ParseIntegerString(read.value, &current)) {
            invalid_integer = true;
            return std::nullopt;
          }
          deadline_us = read.deadline_us;
        }

        const bool ok =
            mode == IntegerMutationMode::kAdd
                ? CheckedAddInt64(current, operand, &next_value)
                : CheckedSubtractInt64(current, operand, &next_value);
        if (!ok) {
          overflow = true;
          return std::nullopt;
        }

        cache::RedisObject object =
            cache::RedisObject::MakeString(std::to_string(next_value));
        if (deadline_us != 0) {
          object = object.WithDeadline(deadline_us);
        }
        return object;
      },
      MakeRecord(args[0], args), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (invalid_integer) {
    return CommandResult{protocol::Response::Error(kIntegerError)};
  }
  if (overflow) {
    return CommandResult{protocol::Response::Error(kOverflowError)};
  }
  return CommandResult{protocol::Response::Integer(next_value)};
}

}  // namespace

std::optional<std::string> SetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 3) {
    return "ERR wrong number of arguments for 'set' command";
  }
  return std::nullopt;
}

protocol::Response SetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SetCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  SetOptions options;
  if (auto error = ParseSetOptions(args, now_us, &options)) {
    return CommandResult{protocol::Response::Error(*error)};
  }

  bool wrong_type = false;
  bool wrote = false;
  bool condition_failed = false;
  std::optional<std::string> old_value;

  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        const bool exists = existing.has_value();
        if (options.get && exists) {
          StringRead read = ReadStringObject(existing);
          if (read.status == StringReadStatus::kWrongType) {
            wrong_type = true;
            return std::nullopt;
          }
          old_value = std::move(read.value);
        }

        if ((options.nx && exists) || (options.xx && !exists)) {
          condition_failed = true;
          return std::nullopt;
        }

        cache::RedisObject object = cache::RedisObject::MakeString(args[2]);
        const std::uint64_t existing_deadline =
            existing.has_value() ? existing->DeadlineUs() : 0;
        wrote = true;
        return ApplyStringDeadline(std::move(object), options.deadline_us,
                                   options.keep_ttl, existing_deadline);
      },
      MakeRecord("SET", args, cache::BinlogOp::kSet), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (options.get) {
    if (condition_failed || !old_value.has_value()) {
      return CommandResult{protocol::Response::NullBulk()};
    }
    return CommandResult{protocol::Response::BulkString(*old_value)};
  }
  if (!wrote) {
    return CommandResult{protocol::Response::NullBulk()};
  }
  return CommandResult{protocol::Response::SimpleString("OK")};
}

std::optional<std::string> GetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'get' command";
  }
  return std::nullopt;
}

protocol::Response GetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  StringRead read = ReadStringObject(engine, args[1], now_us);
  if (read.status == StringReadStatus::kMissing) {
    return protocol::Response::NullBulk();
  }
  if (read.status == StringReadStatus::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::BulkString(std::move(read.value));
}

std::optional<std::string> MGetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 2) {
    return "ERR wrong number of arguments for 'mget' command";
  }
  return std::nullopt;
}

protocol::Response MGetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::vector<protocol::Response> elements;
  elements.reserve(args.size() - 1);
  for (std::size_t i = 1; i < args.size(); ++i) {
    StringRead read = ReadStringObject(engine, args[i], now_us);
    if (read.status == StringReadStatus::kOk) {
      elements.push_back(protocol::Response::BulkString(std::move(read.value)));
    } else {
      elements.push_back(protocol::Response::NullBulk());
    }
  }
  return protocol::Response::Array(std::move(elements));
}

std::optional<std::string> SetNxCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'setnx' command";
  }
  return std::nullopt;
}

protocol::Response SetNxCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult SetNxCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  bool set = false;
  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        if (existing.has_value()) {
          return std::nullopt;
        }
        set = true;
        return cache::RedisObject::MakeString(args[2]);
      },
      MakeRecord("SETNX", args), now_us);
  return CommandResult{protocol::Response::Integer(set ? 1 : 0)};
}

std::optional<std::string> GetSetCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'getset' command";
  }
  return std::nullopt;
}

protocol::Response GetSetCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult GetSetCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  bool wrong_type = false;
  std::optional<std::string> old_value;
  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        if (existing.has_value()) {
          StringRead read = ReadStringObject(existing);
          if (read.status == StringReadStatus::kWrongType) {
            wrong_type = true;
            return std::nullopt;
          }
          old_value = std::move(read.value);
        }
        return cache::RedisObject::MakeString(args[2]);
      },
      MakeRecord("GETSET", args), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  if (!old_value.has_value()) {
    return CommandResult{protocol::Response::NullBulk()};
  }
  return CommandResult{protocol::Response::BulkString(*old_value)};
}

std::optional<std::string> StrLenCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'strlen' command";
  }
  return std::nullopt;
}

protocol::Response StrLenCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  StringRead read = ReadStringObject(engine, args[1], now_us);
  if (read.status == StringReadStatus::kMissing) {
    return protocol::Response::Integer(0);
  }
  if (read.status == StringReadStatus::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(
      static_cast<std::int64_t>(read.value.size()));
}

std::optional<std::string> AppendCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'append' command";
  }
  return std::nullopt;
}

protocol::Response AppendCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult AppendCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  bool wrong_type = false;
  std::int64_t length = 0;
  engine.Update(
      args[1],
      [&](std::optional<cache::RedisObject> existing)
          -> std::optional<cache::RedisObject> {
        std::string next;
        std::uint64_t deadline_us = 0;
        if (existing.has_value()) {
          StringRead read = ReadStringObject(existing);
          if (read.status == StringReadStatus::kWrongType) {
            wrong_type = true;
            return std::nullopt;
          }
          next = std::move(read.value);
          deadline_us = read.deadline_us;
        }
        next.append(args[2].data(), args[2].size());
        length = static_cast<std::int64_t>(next.size());
        cache::RedisObject object = cache::RedisObject::MakeString(next);
        if (deadline_us != 0) {
          object = object.WithDeadline(deadline_us);
        }
        return object;
      },
      MakeRecord("APPEND", args), now_us);

  if (wrong_type) {
    return CommandResult{protocol::Response::Error(kWrongTypeError)};
  }
  return CommandResult{protocol::Response::Integer(length)};
}

std::optional<std::string> IncrCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'incr' command";
  }
  return std::nullopt;
}

protocol::Response IncrCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult IncrCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecIntegerMutation(args, engine, now_us, 1, IntegerMutationMode::kAdd);
}

std::optional<std::string> DecrCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 2) {
    return "ERR wrong number of arguments for 'decr' command";
  }
  return std::nullopt;
}

protocol::Response DecrCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult DecrCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecIntegerMutation(args, engine, now_us, 1,
                             IntegerMutationMode::kSubtract);
}

std::optional<std::string> IncrByCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'incrby' command";
  }
  return std::nullopt;
}

protocol::Response IncrByCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult IncrByCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t delta = 0;
  if (!ParseIntegerString(args[2], &delta)) {
    return CommandResult{protocol::Response::Error(kIntegerError)};
  }
  return ExecIntegerMutation(args, engine, now_us, delta,
                             IntegerMutationMode::kAdd);
}

std::optional<std::string> DecrByCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() != 3) {
    return "ERR wrong number of arguments for 'decrby' command";
  }
  return std::nullopt;
}

protocol::Response DecrByCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  return ExecWithResult(args, engine, now_us).response;
}

CommandResult DecrByCmd::ExecWithResult(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::int64_t amount = 0;
  if (!ParseIntegerString(args[2], &amount)) {
    return CommandResult{protocol::Response::Error(kIntegerError)};
  }
  return ExecIntegerMutation(args, engine, now_us, amount,
                             IntegerMutationMode::kSubtract);
}

}  // namespace command
