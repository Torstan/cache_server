#include "test_harness.h"

#include <cstdint>
#include <limits>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "command/hash_cmd.h"
#include "command/key_cmd.h"
#include "command/set_cmd.h"
#include "command/string_cmd.h"
#include "command/zset_cmd.h"
#include "protocol/response.h"

namespace {

void RequireType(const protocol::Response& response,
                 protocol::ResponseType expected, std::string_view message) {
  test::Require(response.type == expected, message);
}

}  // namespace

CACHE_TEST(StringAndKeyCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;

  command::SetCmd set_cmd("k", "v");
  auto set_response = set_cmd.ExecCmd(engine, now_us);
  RequireType(set_response, protocol::ResponseType::kSimpleString,
              "SET returns a simple string");
  test::RequireEqual(set_response.text, "OK", "SET returns OK");

  command::GetCmd get_cmd("k");
  auto get_response = get_cmd.ExecCmd(engine, now_us);
  RequireType(get_response, protocol::ResponseType::kBulkString,
              "GET returns a bulk string");
  test::RequireEqual(get_response.text, "v", "GET returns value");

  command::TtlCmd ttl_no_expire("k");
  auto ttl_no_expire_response = ttl_no_expire.ExecCmd(engine, now_us);
  RequireType(ttl_no_expire_response, protocol::ResponseType::kInteger,
              "TTL returns an integer");
  test::Require(ttl_no_expire_response.integer == -1,
                "TTL without expire returns -1");

  command::TtlCmd ttl_missing("missing");
  auto ttl_missing_response = ttl_missing.ExecCmd(engine, now_us);
  RequireType(ttl_missing_response, protocol::ResponseType::kInteger,
              "TTL missing key returns an integer");
  test::Require(ttl_missing_response.integer == -2,
                "TTL missing key returns -2");

  command::ExpireCmd expire("k", 2);
  auto expire_response = expire.ExecCmd(engine, now_us);
  RequireType(expire_response, protocol::ResponseType::kInteger,
              "EXPIRE returns an integer");
  test::Require(expire_response.integer == 1, "EXPIRE existing key returns 1");

  command::TtlCmd ttl("k");
  auto ttl_response = ttl.ExecCmd(engine, now_us + 500'000);
  RequireType(ttl_response, protocol::ResponseType::kInteger,
              "TTL with expire returns an integer");
  test::Require(ttl_response.integer == 1, "TTL floors remaining seconds");

  auto expired_get_response = get_cmd.ExecCmd(engine, now_us + 2'000'000);
  RequireType(expired_get_response, protocol::ResponseType::kNullBulkString,
              "GET expired key returns null bulk");

  command::SetCmd reset_cmd("k", "v2");
  auto reset_response = reset_cmd.ExecCmd(engine, now_us + 2'000'000);
  RequireType(reset_response, protocol::ResponseType::kSimpleString,
              "SET after expire returns a simple string");
  test::Require(ttl.ExecCmd(engine, now_us + 2'000'000).integer == -1,
                "SET clears prior TTL");

  command::ExpireCmd delete_now("k", 0);
  auto delete_now_response = delete_now.ExecCmd(engine, now_us + 2'000'000);
  RequireType(delete_now_response, protocol::ResponseType::kInteger,
              "EXPIRE <= 0 returns an integer");
  test::Require(delete_now_response.integer == 1,
                "EXPIRE <= 0 deletes existing key");
  RequireType(get_cmd.ExecCmd(engine, now_us + 2'000'000),
              protocol::ResponseType::kNullBulkString,
              "GET immediately deleted key returns null bulk");

  command::DelCmd del("k");
  auto missing_del_response = del.ExecCmd(engine, now_us + 2'000'000);
  RequireType(missing_del_response, protocol::ResponseType::kInteger,
              "DEL missing key returns an integer");
  test::Require(missing_del_response.integer == 0, "DEL missing key returns 0");

  command::SetCmd del_setup_cmd("k", "v3");
  (void)del_setup_cmd.ExecCmd(engine, now_us + 2'000'000);
  auto del_response = del.ExecCmd(engine, now_us + 2'000'000);
  RequireType(del_response, protocol::ResponseType::kInteger,
              "DEL returns an integer");
  test::Require(del_response.integer == 1, "DEL removes key");
}

CACHE_TEST(ExpireUsesSaturatedTtlInBinlog) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 0;

  command::SetCmd set_cmd("overflow", "v");
  (void)set_cmd.ExecCmd(engine, now_us);

  command::ExpireCmd expire_cmd("overflow",
                                std::numeric_limits<std::int64_t>::max());
  auto expire_response = expire_cmd.ExecCmd(engine, now_us);
  RequireType(expire_response, protocol::ResponseType::kInteger,
              "overflow EXPIRE returns an integer");
  test::Require(expire_response.integer == 1,
                "overflow EXPIRE succeeds for existing key");

  auto get_response = command::GetCmd("overflow").ExecCmd(engine, now_us);
  RequireType(get_response, protocol::ResponseType::kBulkString,
              "saturated EXPIRE keeps key readable before deadline");
  test::RequireEqual(get_response.text, "v", "saturated EXPIRE keeps value");

  auto logs = engine.SlotForKey("overflow").CopyLogsAfter(0, 10);
  test::Require(logs.size() == 2, "SET and EXPIRE logs are present");
  test::Require(logs[1].op == cache::BinlogOp::kExpire,
                "second log records EXPIRE");
  test::Require(logs[1].remaining_ttl_us ==
                    std::numeric_limits<std::uint64_t>::max(),
                "EXPIRE log stores saturated remaining TTL");
}

CACHE_TEST(HashSetAndZSetCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 10'000;

  test::Require(
      command::HSetCmd("h", "f", "v").ExecCmd(engine, now_us).integer == 1,
      "HSET new field returns 1");
  test::Require(
      command::HSetCmd("h", "f", "v2").ExecCmd(engine, now_us).integer == 0,
      "HSET existing field returns 0");
  test::RequireEqual(
      command::HGetCmd("h", "f").ExecCmd(engine, now_us).text, "v2",
      "HGET returns updated value");

  test::Require(command::SAddCmd("s", "m").ExecCmd(engine, now_us).integer ==
                    1,
                "SADD new member returns 1");
  test::Require(command::SAddCmd("s", "m").ExecCmd(engine, now_us).integer ==
                    0,
                "SADD existing member returns 0");
  test::Require(
      command::SIsMemberCmd("s", "m").ExecCmd(engine, now_us).integer == 1,
      "SISMEMBER returns 1");

  test::Require(
      command::ZAddCmd("z", 1.5, "m").ExecCmd(engine, now_us).integer == 1,
      "ZADD new member returns 1");
  test::Require(
      command::ZAddCmd("z", 2.5, "m").ExecCmd(engine, now_us).integer == 0,
      "ZADD existing member returns 0");
  test::RequireEqual(
      command::ZScoreCmd("z", "m").ExecCmd(engine, now_us).text, "2.5",
      "ZSCORE returns score");
}

CACHE_TEST(CommandDispatcherParsesAndExecutesRespArgs) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1000;

  auto set = dispatcher.Execute({"set", "a", "1"}, engine, now_us);
  test::RequireEqual(set.text, "OK", "dispatcher executes SET");

  auto get = dispatcher.Execute({"GET", "a"}, engine, now_us);
  test::RequireEqual(get.text, "1", "dispatcher executes GET");

  auto hset = dispatcher.Execute({"HSET", "h", "f", "v"}, engine, now_us);
  test::Require(hset.integer == 1, "dispatcher executes HSET");
  auto hget = dispatcher.Execute({"hget", "h", "f"}, engine, now_us);
  test::RequireEqual(hget.text, "v", "dispatcher executes HGET");

  auto sadd = dispatcher.Execute({"SADD", "s", "m"}, engine, now_us);
  test::Require(sadd.integer == 1, "dispatcher executes SADD");
  auto sismember = dispatcher.Execute({"SISMEMBER", "s", "m"}, engine, now_us);
  test::Require(sismember.integer == 1, "dispatcher executes SISMEMBER");

  auto zadd = dispatcher.Execute({"ZADD", "z", "1.5", "m"}, engine, now_us);
  test::Require(zadd.integer == 1, "dispatcher executes ZADD");
  auto zscore = dispatcher.Execute({"ZSCORE", "z", "m"}, engine, now_us);
  test::RequireEqual(zscore.text, "1.5", "dispatcher executes ZSCORE");

  auto expire = dispatcher.Execute({"EXPIRE", "a", "2"}, engine, now_us);
  test::Require(expire.integer == 1, "dispatcher parses EXPIRE integer");
  auto ttl = dispatcher.Execute({"TTL", "a"}, engine, now_us);
  test::Require(ttl.integer == 2, "dispatcher executes TTL");
  auto del = dispatcher.Execute({"DEL", "a"}, engine, now_us);
  test::Require(del.integer == 1, "dispatcher executes DEL");

  auto wrong_arity = dispatcher.Execute({"GET"}, engine, now_us);
  test::Require(wrong_arity.type == protocol::ResponseType::kError,
                "wrong arity returns error");

  auto invalid_score = dispatcher.Execute({"ZADD", "z", "nan", "m"}, engine,
                                          now_us);
  test::Require(invalid_score.type == protocol::ResponseType::kError,
                "invalid score returns error");

  auto invalid_integer = dispatcher.Execute({"EXPIRE", "a", "oops"}, engine,
                                            now_us);
  test::Require(invalid_integer.type == protocol::ResponseType::kError,
                "invalid integer returns error");

  auto unknown = dispatcher.Execute({"NOPE", "a"}, engine, now_us);
  test::Require(unknown.type == protocol::ResponseType::kError,
                "unknown command returns error");
}
