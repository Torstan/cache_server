#include "command/key_cmd.h"

namespace command {

DelCmd::DelCmd(std::string_view key) : key_(key) {}

protocol::Response DelCmd::ExecCmd(cache::CacheEngine& engine,
                                   std::uint64_t now_us) const {
  return protocol::Response::Integer(engine.Del(key_, now_us).changed ? 1 : 0);
}

ExpireCmd::ExpireCmd(std::string_view key, std::int64_t seconds)
    : key_(key), seconds_(seconds) {}

protocol::Response ExpireCmd::ExecCmd(cache::CacheEngine& engine,
                                      std::uint64_t now_us) const {
  return protocol::Response::Integer(
      engine.Expire(key_, seconds_, now_us) ? 1 : 0);
}

TtlCmd::TtlCmd(std::string_view key) : key_(key) {}

protocol::Response TtlCmd::ExecCmd(cache::CacheEngine& engine,
                                   std::uint64_t now_us) const {
  return protocol::Response::Integer(engine.Ttl(key_, now_us));
}

}  // namespace command
