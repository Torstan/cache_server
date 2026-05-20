#include "command/command_dispatcher.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "command/hash_cmd.h"
#include "command/key_cmd.h"
#include "command/set_cmd.h"
#include "command/string_cmd.h"
#include "command/zset_cmd.h"

namespace command {
namespace {

std::string ToUpperAscii(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    if (ch >= 'a' && ch <= 'z') {
      result.push_back(static_cast<char>(ch - 'a' + 'A'));
    } else {
      result.push_back(static_cast<char>(ch));
    }
  }
  return result;
}

class ErrorCmd : public RedisCmd {
 public:
  explicit ErrorCmd(std::string text) : text_(std::move(text)) {}
  protocol::Response ExecCmd(cache::CacheEngine& engine,
                             std::uint64_t now_us) const override {
    (void)engine;
    (void)now_us;
    return protocol::Response::Error(text_);
  }

 private:
  std::string text_;
};

std::unique_ptr<RedisCmd> Error(std::string text) {
  return std::make_unique<ErrorCmd>(std::move(text));
}

bool ParseInt64(const std::string& text, std::int64_t* out) {
  if (text.empty()) {
    return false;
  }

  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (errno == ERANGE || end == text.c_str() || *end != '\0') {
    return false;
  }
  static_assert(sizeof(long long) >= sizeof(std::int64_t),
                "strtoll result must hold int64");
  *out = static_cast<std::int64_t>(value);
  return true;
}

bool ParseFiniteDouble(const std::string& text, double* out) {
  if (text.empty()) {
    return false;
  }

  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
      !std::isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

bool HasArity(const std::vector<std::string>& args, std::size_t expected) {
  return args.size() == expected;
}

}  // namespace

protocol::Response CommandDispatcher::Execute(
    const std::vector<std::string>& args, cache::CacheEngine& engine,
    std::uint64_t now_us) const {
  std::unique_ptr<RedisCmd> command = Build(args);
  if (!command) {
    return protocol::Response::Error("ERR unknown command");
  }
  return command->ExecCmd(engine, now_us);
}

std::unique_ptr<RedisCmd> CommandDispatcher::Build(
    const std::vector<std::string>& args) const {
  if (args.empty()) {
    return Error("ERR empty command");
  }

  const std::string command = ToUpperAscii(args[0]);

  if (command == "SET") {
    if (!HasArity(args, 3)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<SetCmd>(args[1], args[2]);
  }
  if (command == "GET") {
    if (!HasArity(args, 2)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<GetCmd>(args[1]);
  }
  if (command == "HSET") {
    if (!HasArity(args, 4)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<HSetCmd>(args[1], args[2], args[3]);
  }
  if (command == "HGET") {
    if (!HasArity(args, 3)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<HGetCmd>(args[1], args[2]);
  }
  if (command == "SADD") {
    if (!HasArity(args, 3)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<SAddCmd>(args[1], args[2]);
  }
  if (command == "SISMEMBER") {
    if (!HasArity(args, 3)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<SIsMemberCmd>(args[1], args[2]);
  }
  if (command == "ZADD") {
    if (!HasArity(args, 4)) {
      return Error("ERR wrong number of arguments");
    }
    double score = 0.0;
    if (!ParseFiniteDouble(args[2], &score)) {
      return Error("ERR invalid score");
    }
    return std::make_unique<ZAddCmd>(args[1], score, args[3]);
  }
  if (command == "ZSCORE") {
    if (!HasArity(args, 3)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<ZScoreCmd>(args[1], args[2]);
  }
  if (command == "DEL") {
    if (!HasArity(args, 2)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<DelCmd>(args[1]);
  }
  if (command == "EXPIRE") {
    if (!HasArity(args, 3)) {
      return Error("ERR wrong number of arguments");
    }
    std::int64_t seconds = 0;
    if (!ParseInt64(args[2], &seconds)) {
      return Error("ERR invalid integer");
    }
    return std::make_unique<ExpireCmd>(args[1], seconds);
  }
  if (command == "TTL") {
    if (!HasArity(args, 2)) {
      return Error("ERR wrong number of arguments");
    }
    return std::make_unique<TtlCmd>(args[1]);
  }

  return Error("ERR unknown command");
}

}  // namespace command
