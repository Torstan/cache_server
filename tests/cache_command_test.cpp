#include "test_harness.h"

#include "cache/cache_engine.h"
#include "command/key_cmd.h"
#include "command/string_cmd.h"

CACHE_TEST(StringAndKeyCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;

  command::SetCmd set_cmd("k", "v");
  auto set_response = set_cmd.ExecCmd(engine, now_us);
  test::RequireEqual(set_response.text, "OK", "SET returns OK");

  command::GetCmd get_cmd("k");
  auto get_response = get_cmd.ExecCmd(engine, now_us);
  test::RequireEqual(get_response.text, "v", "GET returns value");

  command::TtlCmd ttl_no_expire("k");
  test::Require(ttl_no_expire.ExecCmd(engine, now_us).integer == -1,
                "TTL without expire returns -1");

  command::ExpireCmd expire("k", 2);
  test::Require(expire.ExecCmd(engine, now_us).integer == 1,
                "EXPIRE existing key returns 1");

  command::TtlCmd ttl("k");
  test::Require(ttl.ExecCmd(engine, now_us + 500'000).integer == 1,
                "TTL floors remaining seconds");

  command::DelCmd del("k");
  test::Require(del.ExecCmd(engine, now_us + 500'000).integer == 1,
                "DEL removes key");
}
