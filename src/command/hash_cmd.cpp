#include "command/hash_cmd.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

HSetCmd::HSetCmd(std::string_view key, std::string_view field,
                 std::string_view value)
    : key_(key), field_(field), value_(value) {}

protocol::Response HSetCmd::ExecCmd(cache::CacheEngine& engine,
                                    std::uint64_t now_us) const {
  const auto result = engine.HSet(key_, field_, value_, now_us);
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::Integer(result.created ? 1 : 0);
}

HGetCmd::HGetCmd(std::string_view key, std::string_view field)
    : key_(key), field_(field) {}

protocol::Response HGetCmd::ExecCmd(cache::CacheEngine& engine,
                                    std::uint64_t now_us) const {
  const auto result = engine.HGet(key_, field_, now_us);
  if (result.status == cache::Status::kOk) {
    return protocol::Response::BulkString(result.value);
  }
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::NullBulk();
}

}  // namespace command
