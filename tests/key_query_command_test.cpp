#include "test_harness.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/response.h"

namespace {

protocol::Response Exec(command::CommandDispatcher& dispatcher,
                        cache::CacheEngine& engine, std::uint64_t now_us,
                        std::initializer_list<std::string_view> args) {
  std::vector<std::string> owned;
  owned.reserve(args.size());
  for (std::string_view arg : args) {
    owned.emplace_back(arg);
  }
  return dispatcher.Execute(owned, engine, now_us);
}

void RequireType(const protocol::Response& response,
                 protocol::ResponseType expected, std::string_view message) {
  test::Require(response.type == expected, message);
}

void RequireInteger(const protocol::Response& response, std::int64_t expected,
                    std::string_view message) {
  RequireType(response, protocol::ResponseType::kInteger, message);
  test::Require(response.integer == expected, message);
}

void RequireSimpleString(const protocol::Response& response,
                         std::string_view expected,
                         std::string_view message) {
  RequireType(response, protocol::ResponseType::kSimpleString, message);
  test::RequireEqual(response.text, expected, message);
}

}  // namespace

CACHE_TEST(ExistsCountsLiveKeysAndDuplicates) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "s", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "h", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  RequireInteger(Exec(dispatcher, engine, now_us, {"EXISTS", "s"}), 1,
                 "EXISTS returns 1 for live key");
  RequireInteger(Exec(dispatcher, engine, now_us, {"EXISTS", "s", "s", "h"}),
                 3, "EXISTS counts duplicate live keys");
  RequireInteger(
      Exec(dispatcher, engine, now_us + 2'000, {"EXISTS", "expired", "none"}),
      0, "EXISTS ignores expired and missing keys");
}

CACHE_TEST(TypeReportsStoredObjectType) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 2'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "str", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "hash", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SADD", "set", "m"});
  (void)Exec(dispatcher, engine, now_us, {"ZADD", "zset", "1", "m"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "str"}),
                      "string", "TYPE reports string");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "hash"}),
                      "hash", "TYPE reports hash");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "set"}), "set",
                      "TYPE reports set");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "zset"}),
                      "zset", "TYPE reports zset");
  RequireSimpleString(Exec(dispatcher, engine, now_us, {"TYPE", "none"}),
                      "none", "TYPE reports missing key");
  RequireSimpleString(
      Exec(dispatcher, engine, now_us + 2'000, {"TYPE", "expired"}), "none",
      "TYPE reports expired key as none");
}

CACHE_TEST(PTtlReportsMillisecondTtl) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 3'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "plain", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "volatile", "v", "PX", "1500"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  RequireInteger(Exec(dispatcher, engine, now_us, {"PTTL", "missing"}), -2,
                 "PTTL missing key returns -2");
  RequireInteger(Exec(dispatcher, engine, now_us, {"PTTL", "plain"}), -1,
                 "PTTL persistent key returns -1");
  const auto pttl =
      Exec(dispatcher, engine, now_us + 400'000, {"PTTL", "volatile"});
  RequireType(pttl, protocol::ResponseType::kInteger,
              "PTTL volatile key returns integer");
  test::Require(pttl.integer >= 1000 && pttl.integer <= 1100,
                "PTTL returns remaining milliseconds");
  RequireInteger(Exec(dispatcher, engine, now_us + 2'000, {"PTTL", "expired"}),
                 -2, "PTTL expired key returns -2");
}

CACHE_TEST(PTtlSaturatesHugeRemainingTtl) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 4'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "huge", "v"});
  const std::string max_seconds =
      std::to_string(std::numeric_limits<std::int64_t>::max());
  RequireInteger(Exec(dispatcher, engine, now_us,
                      {"EXPIRE", "huge", max_seconds}),
                 1, "huge EXPIRE succeeds");
  const std::int64_t expected_pttl = static_cast<std::int64_t>(
      (std::numeric_limits<std::uint64_t>::max() - now_us) / 1000ULL);
  RequireInteger(Exec(dispatcher, engine, now_us, {"PTTL", "huge"}),
                 expected_pttl, "PTTL reports huge remaining TTL");
}
