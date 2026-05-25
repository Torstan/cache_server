#include "test_harness.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "common/hash.h"
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

protocol::Response Exec(command::CommandDispatcher& dispatcher,
                        cache::CacheEngine& engine, std::uint64_t now_us,
                        const std::vector<std::string>& args) {
  return dispatcher.Execute(args, engine, now_us);
}

void RequireArray(const protocol::Response& response,
                  std::string_view message) {
  test::Require(response.type == protocol::ResponseType::kArray, message);
}

std::vector<std::string> ScanKeys(const protocol::Response& response) {
  RequireArray(response, "SCAN returns array");
  test::Require(response.elements.size() == 2, "SCAN returns cursor and keys");
  test::Require(response.elements[0].type == protocol::ResponseType::kBulkString,
                "SCAN cursor is bulk string");
  RequireArray(response.elements[1], "SCAN keys are array");
  std::vector<std::string> keys;
  for (const protocol::Response& key : response.elements[1].elements) {
    test::Require(key.type == protocol::ResponseType::kBulkString,
                  "SCAN key is bulk string");
    keys.push_back(key.text);
  }
  return keys;
}

std::string ScanCursor(const protocol::Response& response) {
  RequireArray(response, "SCAN returns array");
  test::Require(response.elements.size() == 2, "SCAN returns two elements");
  test::Require(response.elements[0].type == protocol::ResponseType::kBulkString,
                "SCAN cursor is bulk string");
  return response.elements[0].text;
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void RequireErrorText(const protocol::Response& response,
                      std::string_view expected, std::string_view message) {
  test::Require(response.type == protocol::ResponseType::kError, message);
  test::RequireEqual(response.text, expected, message);
}

std::pair<std::string, std::string> DistinctSlotKeys() {
  std::string first;
  std::size_t first_slot = 0;
  for (int index = 0; index < 10000; ++index) {
    const std::string candidate = "multi:" + std::to_string(index);
    const std::size_t slot = common::SlotForKey(candidate);
    if (first.empty()) {
      first = candidate;
      first_slot = slot;
      continue;
    }
    if (slot > first_slot + 1) {
      return {first, candidate};
    }
    if (first_slot > slot + 1) {
      return {candidate, first};
    }
  }
  return {"multi:1", "multi:2"};
}

}  // namespace

CACHE_TEST(ScanReturnsAllKeysAcrossStableIteration) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 1'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "scan:string", "v"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "scan:hash", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SADD", "scan:set", "m"});
  (void)Exec(dispatcher, engine, now_us, {"ZADD", "scan:zset", "1", "m"});

  std::set<std::string> seen;
  std::string cursor = "0";
  for (int iteration = 0; iteration < 200000; ++iteration) {
    std::vector<std::string> args = {"SCAN", cursor, "COUNT", "1"};
    auto response = Exec(dispatcher, engine, now_us, args);
    for (const std::string& key : ScanKeys(response)) {
      seen.insert(key);
    }
    cursor = ScanCursor(response);
    if (cursor == "0") {
      break;
    }
  }

  test::Require(seen.count("scan:string") == 1, "SCAN finds string key");
  test::Require(seen.count("scan:hash") == 1, "SCAN finds hash key");
  test::Require(seen.count("scan:set") == 1, "SCAN finds set key");
  test::Require(seen.count("scan:zset") == 1, "SCAN finds zset key");
}

CACHE_TEST(ScanSupportsMatchAndTypeFilters) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 2'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "user:1", "a"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "user:2", "b"});
  (void)Exec(dispatcher, engine, now_us, {"HSET", "user:hash", "f", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "other:1", "c"});

  auto match = Exec(dispatcher, engine, now_us,
                    {"SCAN", "0", "MATCH", "user:*", "COUNT", "100000"});
  const auto match_keys = ScanKeys(match);
  test::Require(Contains(match_keys, "user:1"), "MATCH includes user:1");
  test::Require(Contains(match_keys, "user:2"), "MATCH includes user:2");
  test::Require(Contains(match_keys, "user:hash"), "MATCH includes user:hash");
  test::Require(!Contains(match_keys, "other:1"), "MATCH excludes other:1");

  auto type = Exec(dispatcher, engine, now_us,
                   {"SCAN", "0", "MATCH", "user:*", "TYPE", "string",
                    "COUNT", "100000"});
  const auto type_keys = ScanKeys(type);
  test::Require(Contains(type_keys, "user:1"), "TYPE string includes string");
  test::Require(Contains(type_keys, "user:2"), "TYPE string includes string 2");
  test::Require(!Contains(type_keys, "user:hash"),
                "TYPE string excludes hash");
}

CACHE_TEST(ScanMayScanMultipleSlotsUntilCountThreshold) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 3'000'000;
  const auto [first_key, second_key] = DistinctSlotKeys();
  const std::size_t first_slot = common::SlotForKey(first_key);

  (void)Exec(dispatcher, engine, now_us, {"SET", first_key, "a"});
  (void)Exec(dispatcher, engine, now_us, {"SET", second_key, "b"});

  auto response = Exec(dispatcher, engine, now_us,
                       {"SCAN", std::to_string(first_slot), "COUNT", "2"});
  const auto keys = ScanKeys(response);
  test::Require(keys.size() >= 2,
                "SCAN may scan multiple slots until COUNT keys seen");
  test::Require(Contains(keys, first_key), "SCAN includes first key");
  test::Require(Contains(keys, second_key), "SCAN includes second key");
  test::Require(ScanCursor(response) != std::to_string(first_slot + 1),
                "SCAN cursor advances beyond one slot when needed");
}

CACHE_TEST(ScanSkipsExpiredKeysAndRejectsInvalidSyntax) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 4'000'000;

  (void)Exec(dispatcher, engine, now_us, {"SET", "live", "v"});
  (void)Exec(dispatcher, engine, now_us, {"SET", "expired", "v", "PX", "1"});

  auto response =
      Exec(dispatcher, engine, now_us + 2'000, {"SCAN", "0", "COUNT", "100000"});
  const auto keys = ScanKeys(response);
  test::Require(Contains(keys, "live"), "SCAN includes live key");
  test::Require(!Contains(keys, "expired"), "SCAN skips expired key");

  test::Require(Exec(dispatcher, engine, now_us, {"SCAN"})
                    .type == protocol::ResponseType::kError,
                "SCAN requires cursor");
  RequireErrorText(
      Exec(dispatcher, engine, now_us, {"SCAN", "bad"}),
      "ERR value is not an integer or out of range",
      "SCAN rejects non-integer cursor with Redis integer error");
  test::Require(Exec(dispatcher, engine, now_us,
                     {"SCAN", "0", "COUNT", "bad"})
                    .type == protocol::ResponseType::kError,
                "SCAN rejects non-integer count");
  test::Require(Exec(dispatcher, engine, now_us,
                     {"SCAN", "0", "UNKNOWN", "x"})
                    .type == protocol::ResponseType::kError,
                "SCAN rejects unknown option");
  test::Require(Exec(dispatcher, engine, now_us,
                     {"SCAN", "0", "TYPE", "list"})
                    .type == protocol::ResponseType::kError,
                "SCAN rejects unsupported TYPE filter");
}

CACHE_TEST(ScanDocumentsCursorAndCountEdgeCases) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  const std::uint64_t now_us = 5'000'000;

  auto empty = Exec(dispatcher, engine, now_us, {"SCAN", "0"});
  test::Require(ScanCursor(empty) == "0", "empty SCAN returns terminal cursor");
  test::Require(ScanKeys(empty).empty(), "empty SCAN returns empty key array");

  (void)Exec(dispatcher, engine, now_us, {"SET", "edge", "v"});
  const std::size_t edge_slot = common::SlotForKey("edge");
  auto count_zero = Exec(dispatcher, engine, now_us,
                         {"SCAN", std::to_string(edge_slot), "COUNT", "0"});
  test::Require(Contains(ScanKeys(count_zero), "edge"),
                "COUNT 0 still scans at least one complete slot");

  RequireErrorText(
      Exec(dispatcher, engine, now_us,
           {"SCAN", std::to_string(common::kSlotCount)}),
      "ERR invalid cursor", "slot cursor equal to slot count is invalid");
}
