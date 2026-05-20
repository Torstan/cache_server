#include "command/string_cmd.h"

#include "cache/redis_object.h"

namespace command {
namespace {

constexpr const char* kWrongTypeError =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

}  // namespace

SetCmd::SetCmd(std::string_view key, std::string_view value)
    : key_(key), value_(value) {}

protocol::Response SetCmd::ExecCmd(cache::CacheEngine& engine,
                                   std::uint64_t now_us) const {
  (void)engine.SetString(key_, value_, now_us);
  return protocol::Response::SimpleString("OK");
}

GetCmd::GetCmd(std::string_view key) : key_(key) {}

protocol::Response GetCmd::ExecCmd(cache::CacheEngine& engine,
                                   std::uint64_t now_us) const {
  const auto result = engine.GetString(key_, now_us);
  if (result.status == cache::Status::kOk) {
    return protocol::Response::BulkString(result.value);
  }
  if (result.status == cache::Status::kWrongType) {
    return protocol::Response::Error(kWrongTypeError);
  }
  return protocol::Response::NullBulk();
}

}  // namespace command
