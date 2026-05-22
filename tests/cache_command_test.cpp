#include "test_harness.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/response.h"

namespace {

void RequireType(const protocol::Response& response,
                 protocol::ResponseType expected, std::string_view message) {
  test::Require(response.type == expected, message);
}

protocol::Response Exec(command::CommandDispatcher& dispatcher,
                        cache::CacheEngine& engine,
                        std::uint64_t now_us,
                        std::initializer_list<std::string_view> args) {
  std::vector<std::string> owned_args;
  owned_args.reserve(args.size());
  for (std::string_view arg : args) {
    owned_args.emplace_back(arg);
  }
  return dispatcher.Execute(owned_args, engine, now_us);
}

command::CommandResult ExecResult(
    command::CommandDispatcher& dispatcher, cache::CacheEngine& engine,
    std::uint64_t now_us, std::initializer_list<std::string_view> args) {
  std::vector<std::string> owned_args;
  owned_args.reserve(args.size());
  for (std::string_view arg : args) {
    owned_args.emplace_back(arg);
  }
  return dispatcher.ExecuteWithResult(owned_args, engine, now_us);
}

}  // namespace

CACHE_TEST(StringAndKeyCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  auto set_response = Exec(dispatcher, engine, now_us, {"SET", "k", "v"});
  RequireType(set_response, protocol::ResponseType::kSimpleString,
              "SET returns a simple string");
  test::RequireEqual(set_response.text, "OK", "SET returns OK");

  auto get_response = Exec(dispatcher, engine, now_us, {"GET", "k"});
  RequireType(get_response, protocol::ResponseType::kBulkString,
              "GET returns a bulk string");
  test::RequireEqual(get_response.text, "v", "GET returns value");

  auto ttl_no_expire_response = Exec(dispatcher, engine, now_us, {"TTL", "k"});
  RequireType(ttl_no_expire_response, protocol::ResponseType::kInteger,
              "TTL returns an integer");
  test::Require(ttl_no_expire_response.integer == -1,
                "TTL without expire returns -1");

  auto ttl_missing_response =
      Exec(dispatcher, engine, now_us, {"TTL", "missing"});
  RequireType(ttl_missing_response, protocol::ResponseType::kInteger,
              "TTL missing key returns an integer");
  test::Require(ttl_missing_response.integer == -2,
                "TTL missing key returns -2");

  auto expire_response = Exec(dispatcher, engine, now_us, {"EXPIRE", "k", "2"});
  RequireType(expire_response, protocol::ResponseType::kInteger,
              "EXPIRE returns an integer");
  test::Require(expire_response.integer == 1, "EXPIRE existing key returns 1");

  auto ttl_response = Exec(dispatcher, engine, now_us + 500'000, {"TTL", "k"});
  RequireType(ttl_response, protocol::ResponseType::kInteger,
              "TTL with expire returns an integer");
  test::Require(ttl_response.integer == 1, "TTL floors remaining seconds");

  auto expired_get_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"GET", "k"});
  RequireType(expired_get_response, protocol::ResponseType::kNullBulkString,
              "GET expired key returns null bulk");

  auto reset_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"SET", "k", "v2"});
  RequireType(reset_response, protocol::ResponseType::kSimpleString,
              "SET after expire returns a simple string");
  test::Require(Exec(dispatcher, engine, now_us + 2'000'000, {"TTL", "k"})
                    .integer == -1,
                "SET clears prior TTL");

  auto delete_now_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"EXPIRE", "k", "0"});
  RequireType(delete_now_response, protocol::ResponseType::kInteger,
              "EXPIRE <= 0 returns an integer");
  test::Require(delete_now_response.integer == 1,
                "EXPIRE <= 0 deletes existing key");
  RequireType(Exec(dispatcher, engine, now_us + 2'000'000, {"GET", "k"}),
              protocol::ResponseType::kNullBulkString,
              "GET immediately deleted key returns null bulk");

  auto missing_del_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"DEL", "k"});
  RequireType(missing_del_response, protocol::ResponseType::kInteger,
              "DEL missing key returns an integer");
  test::Require(missing_del_response.integer == 0, "DEL missing key returns 0");

  (void)Exec(dispatcher, engine, now_us + 2'000'000, {"SET", "k", "v3"});
  auto del_response =
      Exec(dispatcher, engine, now_us + 2'000'000, {"DEL", "k"});
  RequireType(del_response, protocol::ResponseType::kInteger,
              "DEL returns an integer");
  test::Require(del_response.integer == 1, "DEL removes key");
}

CACHE_TEST(ExpireUsesSaturatedTtlInBinlog) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 0;

  (void)Exec(dispatcher, engine, now_us, {"SET", "overflow", "v"});

  const std::string max_int =
      std::to_string(std::numeric_limits<std::int64_t>::max());
  auto expire_response =
      Exec(dispatcher, engine, now_us, {"EXPIRE", "overflow", max_int});
  RequireType(expire_response, protocol::ResponseType::kInteger,
              "overflow EXPIRE returns an integer");
  test::Require(expire_response.integer == 1,
                "overflow EXPIRE succeeds for existing key");

  auto get_response = Exec(dispatcher, engine, now_us, {"GET", "overflow"});
  RequireType(get_response, protocol::ResponseType::kBulkString,
              "saturated EXPIRE keeps key readable before deadline");
  test::RequireEqual(get_response.text, "v", "saturated EXPIRE keeps value");

  auto logs = engine.SlotForKey("overflow").CopyLogsAfter(0, 10);
  test::Require(logs.size() == 2, "SET and EXPIRE logs are present");
  test::Require(logs[1].op.has_value() &&
                    *logs[1].op == cache::BinlogOp::kExpire,
                "second log records EXPIRE");
  test::Require(logs[1].remaining_ttl_us ==
                    std::numeric_limits<std::uint64_t>::max(),
                "EXPIRE log stores saturated remaining TTL");
}

CACHE_TEST(CommandDispatcherReplaysExpireUsingRemainingTtlMetadata) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "ttl", "v"});

  auto client_missing_arg = Exec(dispatcher, engine, now_us, {"EXPIRE", "ttl"});
  test::Require(client_missing_arg.type == protocol::ResponseType::kError,
                "normal EXPIRE still requires seconds argument");

  command::CommandReplayOptions replay_options;
  replay_options.remaining_ttl_us = std::numeric_limits<std::uint64_t>::max();
  std::vector<std::string> replay_args = {"EXPIRE", "ttl"};
  auto replay =
      dispatcher.ExecuteWithResult(replay_args, engine, now_us, replay_options);

  RequireType(replay.response, protocol::ResponseType::kInteger,
              "replayed EXPIRE returns an integer");
  test::Require(replay.response.integer == 1, "replayed EXPIRE succeeds");
  test::Require(replay.wrote, "replayed EXPIRE reports write");
  test::Require(Exec(dispatcher, engine, now_us + 2'000'000, {"GET", "ttl"})
                    .type == protocol::ResponseType::kBulkString,
                "saturated replay TTL preserves key");
}

CACHE_TEST(HashSetAndZSetCommandsMatchRedisSubset) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 10'000;

  test::Require(Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v"})
                    .integer == 1,
                "HSET new field returns 1");
  test::Require(Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v2"})
                    .integer == 0,
                "HSET existing field returns 0");
  test::RequireEqual(
      Exec(dispatcher, engine, now_us, {"HGET", "h", "f"}).text, "v2",
      "HGET returns updated value");

  test::Require(Exec(dispatcher, engine, now_us, {"SADD", "s", "m"}).integer ==
                    1,
                "SADD new member returns 1");
  test::Require(Exec(dispatcher, engine, now_us, {"SADD", "s", "m"}).integer ==
                    0,
                "SADD existing member returns 0");
  test::Require(Exec(dispatcher, engine, now_us, {"SISMEMBER", "s", "m"})
                    .integer == 1,
                "SISMEMBER returns 1");

  test::Require(Exec(dispatcher, engine, now_us, {"ZADD", "z", "1.5", "m"})
                    .integer == 1,
                "ZADD new member returns 1");
  test::Require(Exec(dispatcher, engine, now_us, {"ZADD", "z", "2.5", "m"})
                    .integer == 0,
                "ZADD existing member returns 0");
  test::RequireEqual(
      Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "m"}).text, "2.5",
      "ZSCORE returns score");
}

CACHE_TEST(CommandDispatcherParsesAndExecutesRespArgs) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1000;

  auto set = Exec(dispatcher, engine, now_us, {"set", "a", "1"});
  test::RequireEqual(set.text, "OK", "dispatcher executes SET");

  auto get = Exec(dispatcher, engine, now_us, {"GET", "a"});
  test::RequireEqual(get.text, "1", "dispatcher executes GET");

  std::vector<std::string_view> get_views = {"GET", "a"};
  auto get_view = dispatcher.Execute(get_views, engine, now_us);
  test::RequireEqual(get_view.text, "1", "string_view overload executes GET");

  auto hset = Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v"});
  test::Require(hset.integer == 1, "dispatcher executes HSET");
  auto hget = Exec(dispatcher, engine, now_us, {"hget", "h", "f"});
  test::RequireEqual(hget.text, "v", "dispatcher executes HGET");

  auto sadd = Exec(dispatcher, engine, now_us, {"SADD", "s", "m"});
  test::Require(sadd.integer == 1, "dispatcher executes SADD");
  auto sismember = Exec(dispatcher, engine, now_us, {"SISMEMBER", "s", "m"});
  test::Require(sismember.integer == 1, "dispatcher executes SISMEMBER");

  auto zadd = Exec(dispatcher, engine, now_us, {"ZADD", "z", "1.5", "m"});
  test::Require(zadd.integer == 1, "dispatcher executes ZADD");
  auto zscore = Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "m"});
  test::RequireEqual(zscore.text, "1.5", "dispatcher executes ZSCORE");

  auto expire = Exec(dispatcher, engine, now_us, {"EXPIRE", "a", "2"});
  test::Require(expire.integer == 1, "dispatcher parses EXPIRE integer");
  auto ttl = Exec(dispatcher, engine, now_us, {"TTL", "a"});
  test::Require(ttl.integer == 2, "dispatcher executes TTL");
  auto del = Exec(dispatcher, engine, now_us, {"DEL", "a"});
  test::Require(del.integer == 1, "dispatcher executes DEL");

  auto wrong_arity = Exec(dispatcher, engine, now_us, {"GET"});
  test::Require(wrong_arity.type == protocol::ResponseType::kError,
                "wrong arity returns error");

  auto invalid_score =
      Exec(dispatcher, engine, now_us, {"ZADD", "z", "nan", "m"});
  test::Require(invalid_score.type == protocol::ResponseType::kError,
                "invalid score returns error");

  auto invalid_integer =
      Exec(dispatcher, engine, now_us, {"EXPIRE", "a", "oops"});
  test::Require(invalid_integer.type == protocol::ResponseType::kError,
                "invalid integer returns error");

  auto unknown = Exec(dispatcher, engine, now_us, {"NOPE", "a"});
  test::Require(unknown.type == protocol::ResponseType::kError,
                "unknown command returns error");
}

CACHE_TEST(CommandDispatcherReportsWriteResults) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1000;

  auto set = ExecResult(dispatcher, engine, now_us, {"SET", "k", "v"});
  test::Require(set.wrote, "SET reports write");

  auto get = ExecResult(dispatcher, engine, now_us, {"GET", "k"});
  test::Require(!get.wrote, "GET reports no write");

  auto sadd1 = ExecResult(dispatcher, engine, now_us, {"SADD", "s", "m"});
  test::Require(sadd1.wrote, "new SADD reports write");
  auto sadd2 = ExecResult(dispatcher, engine, now_us, {"SADD", "s", "m"});
  test::Require(!sadd2.wrote, "duplicate SADD reports no write");

  auto missing_del = ExecResult(dispatcher, engine, now_us, {"DEL", "missing"});
  test::Require(!missing_del.wrote, "missing DEL reports no write");

  auto missing_expire =
      ExecResult(dispatcher, engine, now_us, {"EXPIRE", "missing", "10"});
  test::Require(!missing_expire.wrote, "missing EXPIRE reports no write");
  auto expire = ExecResult(dispatcher, engine, now_us, {"EXPIRE", "k", "10"});
  test::Require(expire.wrote, "existing EXPIRE reports write");
}
