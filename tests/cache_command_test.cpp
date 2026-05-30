#include "test_harness.h"

#include <cstdint>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <algorithm>
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

void RequireInteger(const protocol::Response& response, std::int64_t expected,
                    std::string_view message) {
  RequireType(response, protocol::ResponseType::kInteger, message);
  test::Require(response.integer == expected, message);
}

void RequireBulk(const protocol::Response& response, std::string_view expected,
                 std::string_view message) {
  RequireType(response, protocol::ResponseType::kBulkString, message);
  test::RequireEqual(response.text, expected, message);
}

void RequireNullBulk(const protocol::Response& response,
                     std::string_view message) {
  RequireType(response, protocol::ResponseType::kNullBulkString, message);
}

void RequireSimpleString(const protocol::Response& response,
                         std::string_view expected,
                         std::string_view message) {
  RequireType(response, protocol::ResponseType::kSimpleString, message);
  test::RequireEqual(response.text, expected, message);
}

void RequireErrorContaining(const protocol::Response& response,
                            std::string_view expected,
                            std::string_view message) {
  RequireType(response, protocol::ResponseType::kError, message);
  test::Require(response.text.find(expected) != std::string::npos, message);
}

void RequireArrayTexts(const protocol::Response& response,
                       std::initializer_list<std::string_view> expected,
                       std::string_view message) {
  RequireType(response, protocol::ResponseType::kArray, message);
  test::Require(response.elements.size() == expected.size(), message);
  std::size_t index = 0;
  for (std::string_view text : expected) {
    const protocol::Response& element = response.elements[index++];
    RequireType(element, protocol::ResponseType::kBulkString, message);
    test::RequireEqual(element.text, text, message);
  }
}

void RequireArrayIntegers(const protocol::Response& response,
                          std::initializer_list<std::int64_t> expected,
                          std::string_view message) {
  RequireType(response, protocol::ResponseType::kArray, message);
  test::Require(response.elements.size() == expected.size(), message);
  std::size_t index = 0;
  for (std::int64_t value : expected) {
    const protocol::Response& element = response.elements[index++];
    RequireType(element, protocol::ResponseType::kInteger, message);
    test::Require(element.integer == value, message);
  }
}

bool ArrayHasBulkText(const protocol::Response& response,
                      std::string_view expected) {
  if (response.type != protocol::ResponseType::kArray) {
    return false;
  }
  return std::any_of(response.elements.begin(), response.elements.end(),
                     [&](const protocol::Response& element) {
                       return element.type == protocol::ResponseType::kBulkString &&
                              element.text == expected;
                     });
}

bool SlotHasLogCommand(cache::CacheEngine& engine, std::string_view key,
                       std::string_view command) {
  auto records = engine.SlotForKey(key).CopyLogsAfter(0, 100);
  return std::any_of(records.begin(), records.end(),
                     [&](const cache::BinlogRecord& record) {
                       return !record.args.empty() && record.args[0] == command;
                     });
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

  (void)Exec(dispatcher, engine, now_us + 2'000'000, {"SET", "k1", "v"});
  (void)Exec(dispatcher, engine, now_us + 2'000'000, {"SET", "k2", "v"});
  auto multi_del_response = Exec(dispatcher, engine, now_us + 2'000'000,
                                 {"DEL", "k1", "missing", "k2"});
  RequireInteger(multi_del_response, 2, "DEL removes multiple keys");
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

CACHE_TEST(StringCommandsMatchRedis62CoreSemantics) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  RequireSimpleString(
      Exec(dispatcher, engine, now_us, {"SET", "s", "one", "EX", "10"}),
      "OK", "SET accepts EX");
  RequireBulk(Exec(dispatcher, engine, now_us, {"SET", "s", "two", "GET"}),
              "one", "SET GET returns old value");
  RequireInteger(Exec(dispatcher, engine, now_us, {"TTL", "s"}), -1,
                 "SET without KEEPTTL clears TTL");

  RequireSimpleString(
      Exec(dispatcher, engine, now_us, {"SET", "s", "ttl", "EX", "10"}),
      "OK", "SET resets value with TTL");
  RequireType(
      Exec(dispatcher, engine, now_us, {"SET", "s", "blocked", "NX", "GET"}),
      protocol::ResponseType::kError,
      "Redis 6.2 rejects SET NX GET combination");
  RequireNullBulk(
      Exec(dispatcher, engine, now_us, {"SET", "s", "blocked", "NX"}),
      "SET NX returns null bulk when condition fails");
  RequireBulk(Exec(dispatcher, engine, now_us, {"GET", "s"}), "ttl",
              "SET NX does not overwrite existing key");
  RequireSimpleString(
      Exec(dispatcher, engine, now_us, {"SET", "s", "kept", "XX", "KEEPTTL"}),
      "OK", "SET XX KEEPTTL updates existing key");
  test::Require(Exec(dispatcher, engine, now_us, {"TTL", "s"}).integer > 0,
                "SET KEEPTTL preserves TTL");

  const auto unix_now_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  RequireSimpleString(
      Exec(dispatcher, engine, now_us,
           {"SET", "abs-sec", "v", "EXAT",
            std::to_string(unix_now_seconds + 10)}),
      "OK", "SET accepts EXAT");
  const auto exat_ttl = Exec(dispatcher, engine, now_us, {"TTL", "abs-sec"});
  RequireType(exat_ttl, protocol::ResponseType::kInteger,
              "SET EXAT stores a TTL");
  test::Require(exat_ttl.integer >= 5 && exat_ttl.integer <= 10,
                "SET EXAT translates unix time to relative TTL");

  const auto unix_now_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  RequireSimpleString(
      Exec(dispatcher, engine, now_us,
           {"SET", "abs-ms", "v", "PXAT",
            std::to_string(unix_now_milliseconds + 10000)}),
      "OK", "SET accepts PXAT");
  const auto pxat_ttl = Exec(dispatcher, engine, now_us, {"TTL", "abs-ms"});
  RequireType(pxat_ttl, protocol::ResponseType::kInteger,
              "SET PXAT stores a TTL");
  test::Require(pxat_ttl.integer >= 5 && pxat_ttl.integer <= 10,
                "SET PXAT translates unix time to relative TTL");

  RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "s", "no"}), 0,
                 "SETNX existing key returns 0");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "new", "yes"}), 1,
                 "SETNX missing key returns 1");
  auto mget = Exec(dispatcher, engine, now_us, {"MGET", "s", "missing", "new"});
  RequireType(mget, protocol::ResponseType::kArray,
              "MGET returns an array");
  test::Require(mget.elements.size() == 3, "MGET returns one item per key");
  RequireBulk(mget.elements[0], "kept", "MGET returns first value");
  RequireNullBulk(mget.elements[1], "MGET missing element is null bulk");
  RequireBulk(mget.elements[2], "yes", "MGET returns last value");

  RequireBulk(Exec(dispatcher, engine, now_us, {"GETSET", "new", "old"}),
              "yes", "GETSET returns old value");
  RequireBulk(Exec(dispatcher, engine, now_us, {"GET", "new"}), "old",
              "GETSET stores new value");
  RequireInteger(Exec(dispatcher, engine, now_us, {"APPEND", "new", "-tail"}),
                 8, "APPEND returns new string length");
  RequireInteger(Exec(dispatcher, engine, now_us, {"STRLEN", "new"}), 8,
                 "STRLEN returns string length");
  RequireBulk(Exec(dispatcher, engine, now_us, {"GET", "new"}), "old-tail",
              "APPEND changes string value");

  RequireInteger(Exec(dispatcher, engine, now_us, {"INCR", "counter"}), 1,
                 "INCR creates integer string");
  RequireInteger(Exec(dispatcher, engine, now_us, {"INCRBY", "counter", "9"}),
                 10, "INCRBY adds delta");
  RequireInteger(Exec(dispatcher, engine, now_us, {"DECR", "counter"}), 9,
                 "DECR subtracts one");
  RequireInteger(Exec(dispatcher, engine, now_us, {"DECRBY", "counter", "4"}),
                 5, "DECRBY subtracts delta");

  RequireSimpleString(
      Exec(dispatcher, engine, now_us, {"SET", "bad-int", "abc"}), "OK",
      "SET stores non-integer string");
  RequireType(Exec(dispatcher, engine, now_us, {"INCR", "bad-int"}),
              protocol::ResponseType::kError,
              "INCR rejects non-integer strings");
  RequireSimpleString(
      Exec(dispatcher, engine, now_us, {"SET", "bad-leading", " 11"}), "OK",
      "SET stores leading-space integer string");
  RequireErrorContaining(
      Exec(dispatcher, engine, now_us, {"INCR", "bad-leading"}),
      "not an integer", "INCR rejects leading whitespace");
  RequireErrorContaining(
      Exec(dispatcher, engine, now_us,
           {"SET", "too-far", "v", "EX", "10000000000000000"}),
      "invalid expire time in set",
      "SET EX rejects expiration that overflows Redis expire storage");
}

CACHE_TEST(HashCommandsMatchRedis62CoreSemantics) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 2'000'000;

  RequireInteger(
      Exec(dispatcher, engine, now_us, {"HSET", "h", "a", "1", "b", "two"}),
      2, "HSET accepts multiple field-value pairs");
  RequireInteger(
      Exec(dispatcher, engine, now_us, {"HSET", "h", "a", "3", "c", "4"}), 1,
      "HSET counts only newly added fields");
  RequireSimpleString(
      Exec(dispatcher, engine, now_us, {"HMSET", "h", "d", "5", "e", "6"}),
      "OK", "HMSET returns OK");
  auto hmget = Exec(dispatcher, engine, now_us,
                    {"HMGET", "h", "a", "missing", "d"});
  RequireType(hmget, protocol::ResponseType::kArray,
              "HMGET returns an array");
  test::Require(hmget.elements.size() == 3,
                "HMGET returns one item per field");
  RequireBulk(hmget.elements[0], "3", "HMGET returns first field");
  RequireNullBulk(hmget.elements[1], "HMGET missing field is null bulk");
  RequireBulk(hmget.elements[2], "5", "HMGET returns last field");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HEXISTS", "h", "b"}), 1,
                 "HEXISTS returns 1 for existing field");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HLEN", "h"}), 5,
                 "HLEN counts fields");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HSTRLEN", "h", "b"}), 3,
                 "HSTRLEN returns value length");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HINCRBY", "h", "a", "7"}),
                 10, "HINCRBY updates integer field");
  RequireBulk(Exec(dispatcher, engine, now_us, {"HGET", "h", "a"}), "10",
              "HINCRBY stores updated integer");
  (void)Exec(dispatcher, engine, now_us,
             {"HSET", "hincr-space", "space", " 11"});
  RequireErrorContaining(
      Exec(dispatcher, engine, now_us,
           {"HINCRBY", "hincr-space", "space", "1"}),
      "not an integer", "HINCRBY rejects leading whitespace");

  auto keys = Exec(dispatcher, engine, now_us, {"HKEYS", "h"});
  test::Require(ArrayHasBulkText(keys, "a") && ArrayHasBulkText(keys, "e"),
                "HKEYS returns hash fields");
  auto values = Exec(dispatcher, engine, now_us, {"HVALS", "h"});
  test::Require(ArrayHasBulkText(values, "10") && ArrayHasBulkText(values, "6"),
                "HVALS returns hash values");
  auto all = Exec(dispatcher, engine, now_us, {"HGETALL", "h"});
  test::Require(ArrayHasBulkText(all, "a") && ArrayHasBulkText(all, "10"),
                "HGETALL returns field-value entries");

  RequireInteger(
      Exec(dispatcher, engine, now_us, {"HDEL", "h", "a", "missing", "b"}),
      2, "HDEL removes existing fields only");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HLEN", "h"}), 3,
                 "HDEL updates field count");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HDEL", "single", "x"}), 0,
                 "HDEL missing key returns 0");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HSET", "single", "x", "y"}),
                 1, "HSET creates single-field hash");
  RequireInteger(Exec(dispatcher, engine, now_us, {"HDEL", "single", "x"}), 1,
                 "HDEL removes final field");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "single", "str"}),
                 1, "removing final hash field deletes key");
}

CACHE_TEST(SetCommandsMatchRedis62CoreSemantics) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 3'000'000;

  RequireInteger(Exec(dispatcher, engine, now_us,
                      {"SADD", "s", "a", "b", "c", "a"}),
                 3, "SADD accepts multiple members and counts new ones");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SCARD", "s"}), 3,
                 "SCARD returns set cardinality");
  RequireArrayIntegers(
      Exec(dispatcher, engine, now_us, {"SMISMEMBER", "s", "a", "x", "c"}),
      {1, 0, 1}, "SMISMEMBER returns membership flags");
  auto members = Exec(dispatcher, engine, now_us, {"SMEMBERS", "s"});
  test::Require(ArrayHasBulkText(members, "a") && ArrayHasBulkText(members, "c"),
                "SMEMBERS returns set members");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SREM", "s", "b", "x"}), 1,
                 "SREM removes existing members only");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SCARD", "s"}), 2,
                 "SREM updates cardinality");

  auto random_one = Exec(dispatcher, engine, now_us, {"SRANDMEMBER", "s"});
  RequireType(random_one, protocol::ResponseType::kBulkString,
              "SRANDMEMBER without count returns one bulk string");
  auto random_many = Exec(dispatcher, engine, now_us, {"SRANDMEMBER", "s", "3"});
  RequireType(random_many, protocol::ResponseType::kArray,
              "SRANDMEMBER with count returns array");
  test::Require(random_many.elements.size() <= 2,
                "positive SRANDMEMBER count does not exceed cardinality");
  RequireErrorContaining(
      Exec(dispatcher, engine, now_us,
           {"SRANDMEMBER", "s", "-9223372036854775808"}),
      "value is out of range",
      "SRANDMEMBER rejects minimum int64 count as out of range");
  auto popped = Exec(dispatcher, engine, now_us, {"SPOP", "s", "2"});
  RequireType(popped, protocol::ResponseType::kArray,
              "SPOP with count returns array");
  test::Require(popped.elements.size() == 2, "SPOP removes requested members");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SCARD", "s"}), 0,
                 "SPOP removes popped members");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "s", "str"}), 1,
                 "removing final set member deletes key");
}

CACHE_TEST(ZSetCommandsMatchRedis62CoreSemantics) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 4'000'000;

  RequireInteger(Exec(dispatcher, engine, now_us,
                      {"ZADD", "z", "1", "one", "2", "two", "3", "three"}),
                 3, "ZADD accepts multiple score-member pairs");
  RequireInteger(Exec(dispatcher, engine, now_us,
                      {"ZADD", "z", "NX", "10", "one", "4", "four"}),
                 1, "ZADD NX only adds new members");
  RequireBulk(Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "one"}), "1",
              "ZADD NX does not update existing member");
  RequireInteger(Exec(dispatcher, engine, now_us,
                      {"ZADD", "z", "XX", "CH", "5", "two", "6", "missing"}),
                 1, "ZADD XX CH counts changed existing members");
  RequireBulk(Exec(dispatcher, engine, now_us, {"ZSCORE", "z", "two"}), "5",
              "ZADD XX updates existing member");
  RequireInteger(
      Exec(dispatcher, engine, now_us, {"ZADD", "z", "GT", "CH", "4", "one"}),
      1, "ZADD GT updates only greater scores");
  RequireInteger(
      Exec(dispatcher, engine, now_us, {"ZADD", "z", "LT", "CH", "3", "one"}),
      1, "ZADD LT updates only lower scores");
  RequireBulk(
      Exec(dispatcher, engine, now_us, {"ZADD", "z", "INCR", "2", "one"}), "5",
      "ZADD INCR returns incremented score");
  RequireBulk(Exec(dispatcher, engine, now_us, {"ZINCRBY", "z", "1.5", "one"}),
              "6.5", "ZINCRBY increments score");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZCARD", "z"}), 4,
                 "ZCARD returns cardinality");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZRANK", "z", "three"}), 0,
                 "ZRANK returns zero-based rank");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZREVRANK", "z", "one"}), 0,
                 "ZREVRANK returns reverse rank");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZCOUNT", "z", "(3", "+inf"}),
                 3, "ZCOUNT supports exclusive and infinity bounds");

  RequireArrayTexts(Exec(dispatcher, engine, now_us,
                         {"ZRANGE", "z", "0", "-1", "WITHSCORES"}),
                    {"three", "3", "four", "4", "two", "5", "one", "6.5"},
                    "ZRANGE rank WITHSCORES returns ordered members and scores");
  RequireArrayTexts(Exec(dispatcher, engine, now_us,
                         {"ZRANGE", "z", "+inf", "(4", "BYSCORE", "REV",
                          "LIMIT", "0", "2"}),
                    {"one", "two"}, "ZRANGE BYSCORE REV LIMIT works");
  RequireArrayTexts(Exec(dispatcher, engine, now_us,
                         {"ZRANGE", "z", "[o", "+", "BYLEX"}),
                    {"one", "three", "two"},
                    "ZRANGE BYLEX filters lexicographic members");

  RequireInteger(Exec(dispatcher, engine, now_us, {"ZREM", "z", "one", "none"}),
                 1, "ZREM removes existing members only");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZCARD", "z"}), 3,
                 "ZREM updates cardinality");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZADD", "single-z", "1", "m"}),
                 1, "ZADD creates single-member sorted set");
  RequireInteger(Exec(dispatcher, engine, now_us, {"ZREM", "single-z", "m"}), 1,
                 "ZREM removes final member");
  RequireInteger(Exec(dispatcher, engine, now_us, {"SETNX", "single-z", "str"}),
                 1, "removing final zset member deletes key");
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

CACHE_TEST(CommandDispatcherClassifiesWriteCommands) {
  command::CommandDispatcher dispatcher;

  test::Require(dispatcher.IsWriteCommand("SET"), "SET is write command");
  test::Require(dispatcher.IsWriteCommand("set"),
                "command classification is case-insensitive");
  test::Require(dispatcher.IsWriteCommand("HSET"), "HSET is write command");
  test::Require(dispatcher.IsWriteCommand("SADD"), "SADD is write command");
  test::Require(dispatcher.IsWriteCommand("ZADD"), "ZADD is write command");
  test::Require(dispatcher.IsWriteCommand("DEL"), "DEL is write command");
  test::Require(dispatcher.IsWriteCommand("EXPIRE"),
                "EXPIRE is write command");
  for (std::string_view cmd :
       {"SETNX", "GETSET", "APPEND", "INCR", "DECR", "INCRBY", "DECRBY",
        "HDEL", "HMSET", "HINCRBY", "SREM", "SPOP", "ZREM", "ZINCRBY"}) {
    test::Require(dispatcher.IsWriteCommand(cmd),
                  "new write command is classified");
  }

  test::Require(!dispatcher.IsWriteCommand("GET"), "GET is read command");
  test::Require(!dispatcher.IsWriteCommand("MGET"), "MGET is read command");
  test::Require(!dispatcher.IsWriteCommand("STRLEN"),
                "STRLEN is read command");
  test::Require(!dispatcher.IsWriteCommand("HGET"), "HGET is read command");
  test::Require(!dispatcher.IsWriteCommand("HMGET"), "HMGET is read command");
  test::Require(!dispatcher.IsWriteCommand("HKEYS"), "HKEYS is read command");
  test::Require(!dispatcher.IsWriteCommand("SISMEMBER"),
                "SISMEMBER is read command");
  test::Require(!dispatcher.IsWriteCommand("SMEMBERS"),
                "SMEMBERS is read command");
  test::Require(!dispatcher.IsWriteCommand("SRANDMEMBER"),
                "SRANDMEMBER is read command");
  test::Require(!dispatcher.IsWriteCommand("ZSCORE"),
                "ZSCORE is read command");
  test::Require(!dispatcher.IsWriteCommand("ZRANGE"),
                "ZRANGE is read command");
  test::Require(!dispatcher.IsWriteCommand("TTL"), "TTL is read command");
  test::Require(!dispatcher.IsWriteCommand("NO_SUCH_COMMAND"),
                "unknown command is not classified as write");
}

CACHE_TEST(NewWriteCommandsStoreReplayableArgv) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 5'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "str-log", "a"});
  (void)Exec(dispatcher, engine, now_us, {"APPEND", "str-log", "b"});
  test::Require(SlotHasLogCommand(engine, "str-log", "APPEND"),
                "APPEND stores replayable argv");

  (void)Exec(dispatcher, engine, now_us, {"HSET", "hash-log", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HDEL", "hash-log", "f"});
  test::Require(SlotHasLogCommand(engine, "hash-log", "HDEL"),
                "HDEL stores replayable argv");

  (void)Exec(dispatcher, engine, now_us, {"SADD", "set-log", "m"});
  (void)Exec(dispatcher, engine, now_us, {"SREM", "set-log", "m"});
  test::Require(SlotHasLogCommand(engine, "set-log", "SREM"),
                "SREM stores replayable argv");

  (void)Exec(dispatcher, engine, now_us, {"ZADD", "z-log", "1", "m"});
  (void)Exec(dispatcher, engine, now_us, {"ZINCRBY", "z-log", "2", "m"});
  (void)Exec(dispatcher, engine, now_us, {"ZREM", "z-log", "m"});
  test::Require(SlotHasLogCommand(engine, "z-log", "ZINCRBY"),
                "ZINCRBY stores replayable argv");
  test::Require(SlotHasLogCommand(engine, "z-log", "ZREM"),
                "ZREM stores replayable argv");
}

CACHE_TEST(UnsupportedRedis62CommandsRemainUnregistered) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 6'000'000;

  for (std::string_view cmd :
       {"MSET", "MSETNX", "SETEX", "PSETEX", "GETDEL", "GETEX",
        "GETRANGE", "SETRANGE", "GETBIT", "SETBIT", "BITCOUNT",
        "BITFIELD", "BITFIELD_RO", "BITOP", "BITPOS", "INCRBYFLOAT",
        "HSETNX", "HINCRBYFLOAT", "HRANDFIELD", "HSCAN", "SINTER",
        "SUNION", "SDIFF", "SINTERCARD", "SINTERSTORE", "SUNIONSTORE",
        "SDIFFSTORE", "SMOVE", "SSCAN", "ZMSCORE", "ZRANDMEMBER",
        "ZPOPMIN", "ZPOPMAX", "BZPOPMIN", "BZPOPMAX", "ZUNION",
        "ZINTER", "ZDIFF", "ZRANGEBYSCORE", "ZREVRANGEBYSCORE",
        "ZRANGEBYLEX", "ZREVRANGEBYLEX", "ZREVRANGE",
        "ZREMRANGEBYRANK", "ZREMRANGEBYSCORE", "ZREMRANGEBYLEX",
        "ZLEXCOUNT", "ZRANGESTORE", "ZUNIONSTORE", "ZINTERSTORE",
        "ZDIFFSTORE", "ZSCAN"}) {
    auto response = Exec(dispatcher, engine, now_us, {cmd, "k"});
    RequireType(response, protocol::ResponseType::kError,
                "unsupported command returns error");
    test::Require(response.text.find("ERR unknown command") == 0,
                  "unsupported command remains unregistered");
  }
}

CACHE_TEST(CommandDispatcherClassifiesReadAndWriteCommands) {
  command::CommandDispatcher dispatcher;
  test::Require(dispatcher.IsWriteCommand("SET"), "SET is write");
  test::Require(dispatcher.IsWriteCommand("del"), "DEL is write");
  test::Require(!dispatcher.IsWriteCommand("GET"), "GET is read");
  test::Require(!dispatcher.IsWriteCommand("ttl"), "TTL is read");
  test::Require(!dispatcher.IsWriteCommand("NO_SUCH_COMMAND"),
                "unknown command is not write");
}
