#include "test_harness.h"

#include <cstdint>
#include <string>
#include <utility>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"
#include "common/hash.h"
#include "expire/expire_sweeper.h"

CACHE_TEST(ExpireSweeperDeletesExpiredKeysViaWritePath) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;

  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", "b7p", "v"};
  engine.Set("b7p", cache::RedisObject::MakeString("v"), std::move(record),
             now_us);

  test::Require(engine.Expire("b7p", 1, now_us), "expire succeeds");
  test::Require(common::SlotForKey("b7p") == 0, "test key is in first slot");

  expire::ExpireSweeper sweeper(&engine, 8);
  std::size_t deleted = sweeper.SweepOnce(now_us + 2'000'000);
  test::Require(deleted == 1, "one expired key deleted");
  test::Require(!engine.Get("b7p", now_us + 2'000'000).has_value(),
                "expired key is gone");
}
