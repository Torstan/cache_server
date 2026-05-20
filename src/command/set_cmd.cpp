#include "command/set_cmd.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

SAddCmd::SAddCmd(std::string_view key, std::string_view member)
    : key_(key), member_(member) {}

protocol::Response SAddCmd::ExecCmd(cache::CacheEngine& engine,
                                    std::uint64_t now_us) const {
  const auto result = engine.SAdd(key_, member_, now_us);
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(result.created ? 1 : 0);
}

SIsMemberCmd::SIsMemberCmd(std::string_view key, std::string_view member)
    : key_(key), member_(member) {}

protocol::Response SIsMemberCmd::ExecCmd(cache::CacheEngine& engine,
                                         std::uint64_t now_us) const {
  const auto result = engine.SIsMember(key_, member_, now_us);
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(
      result.status == cache::Status::kOk && result.value ? 1 : 0);
}

}  // namespace command
