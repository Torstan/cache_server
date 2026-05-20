#include "command/zset_cmd.h"

#include <iomanip>
#include <locale>
#include <sstream>

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

ZAddCmd::ZAddCmd(std::string_view key, double score, std::string_view member)
    : key_(key), score_(score), member_(member) {}

protocol::Response ZAddCmd::ExecCmd(cache::CacheEngine& engine,
                                    std::uint64_t now_us) const {
  const auto result = engine.ZAdd(key_, score_, member_, now_us);
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(result.created ? 1 : 0);
}

ZScoreCmd::ZScoreCmd(std::string_view key, std::string_view member)
    : key_(key), member_(member) {}

protocol::Response ZScoreCmd::ExecCmd(cache::CacheEngine& engine,
                                      std::uint64_t now_us) const {
  const auto result = engine.ZScore(key_, member_, now_us);
  if (result.status == cache::Status::kOk) {
    return protocol::Response::BulkString(FormatScore(result.value));
  }
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::NullBulk();
}

}  // namespace command
