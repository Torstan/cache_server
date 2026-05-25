#include "command/scan_cmd.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/glob_match.h"
#include "common/parse_utils.h"

namespace command {
namespace {

constexpr std::size_t kDefaultCount = 1000;

struct ScanOptions {
  std::size_t cursor = 0;
  std::size_t count = kDefaultCount;
  std::optional<std::string> match;
  std::optional<std::string> type;
};

std::string ObjectTypeName(cache::RedisObjectType type) {
  switch (type) {
    case cache::RedisObjectType::kString:
      return "string";
    case cache::RedisObjectType::kHash:
      return "hash";
    case cache::RedisObjectType::kSet:
      return "set";
    case cache::RedisObjectType::kZSet:
      return "zset";
  }
  return "none";
}

std::string ToLowerAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    result.push_back(ch >= 'A' && ch <= 'Z'
                         ? static_cast<char>(ch - 'A' + 'a')
                         : static_cast<char>(ch));
  }
  return result;
}

bool ParseNonNegativeSize(std::string_view text, std::size_t* out) {
  std::int64_t parsed = 0;
  if (!common::ParseInt64(text, &parsed) || parsed < 0) {
    return false;
  }
  *out = static_cast<std::size_t>(parsed);
  return true;
}

bool IsSupportedTypeFilter(std::string_view type) {
  return type == "string" || type == "hash" || type == "set" || type == "zset";
}

std::optional<std::string> ParseOptions(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    ScanOptions* options) {
  if (!ParseNonNegativeSize(args[1], &options->cursor)) {
    return "ERR value is not an integer or out of range";
  }
  if (options->cursor >= engine.SlotCount()) {
    return "ERR invalid cursor";
  }

  for (std::size_t index = 2; index < args.size();) {
    const std::string option = common::ToUpperAscii(args[index]);
    if (option == "MATCH") {
      if (index + 1 >= args.size()) {
        return "ERR syntax error";
      }
      options->match = std::string(args[index + 1]);
      index += 2;
    } else if (option == "COUNT") {
      if (index + 1 >= args.size() ||
          !ParseNonNegativeSize(args[index + 1], &options->count)) {
        return "ERR value is not an integer or out of range";
      }
      index += 2;
    } else if (option == "TYPE") {
      if (index + 1 >= args.size()) {
        return "ERR syntax error";
      }
      options->type = ToLowerAscii(args[index + 1]);
      if (!IsSupportedTypeFilter(*options->type)) {
        return "ERR syntax error";
      }
      index += 2;
    } else {
      return "ERR syntax error";
    }
  }
  return std::nullopt;
}

bool TypeMatches(const std::optional<std::string>& filter,
                 cache::RedisObjectType type) {
  if (!filter.has_value()) {
    return true;
  }
  return *filter == ObjectTypeName(type);
}

bool KeyMatches(const std::optional<std::string>& pattern,
                std::string_view key) {
  if (!pattern.has_value()) {
    return true;
  }
  return common::GlobMatch(*pattern, key);
}

}  // namespace

std::optional<std::string> ScanCmd::CheckArity(
    const std::vector<std::string_view>& args) const {
  if (args.size() < 2) {
    return "ERR wrong number of arguments for 'scan' command";
  }
  return std::nullopt;
}

protocol::Response ScanCmd::ExecCmd(
    const std::vector<std::string_view>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  ScanOptions options;
  if (auto error = ParseOptions(args, engine, &options)) {
    return protocol::Response::Error(*error);
  }

  std::vector<protocol::Response> keys;
  std::size_t scanned_keys = 0;
  std::size_t slot = options.cursor;
  while (slot < engine.SlotCount()) {
    engine.ForEachLiveObjectInSlot(
        slot, now_us,
        [&](const cache::PackedString& key, const cache::RedisObject& object) {
          ++scanned_keys;
          if (TypeMatches(options.type, object.Type()) &&
              KeyMatches(options.match, key.View())) {
            keys.push_back(protocol::Response::BulkString(key.View()));
          }
        });
    ++slot;
    if (scanned_keys >= options.count) {
      break;
    }
  }

  const std::size_t next_cursor = slot >= engine.SlotCount() ? 0 : slot;
  std::vector<protocol::Response> result;
  result.push_back(protocol::Response::BulkString(std::to_string(next_cursor)));
  result.push_back(protocol::Response::Array(std::move(keys)));
  return protocol::Response::Array(std::move(result));
}

}  // namespace command
