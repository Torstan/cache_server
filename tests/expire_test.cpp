#include "test_harness.h"

#include "cache/cache_engine.h"
#include "common/hash.h"
#include "expire/expire_sweeper.h"

CACHE_TEST(ExpireSweeperDeletesExpiredKeysViaWritePath) {
  cache::CacheEngine engine;
  const std::uint64_t now_us = 1'000'000;
  engine.SetString("b7p", "v", now_us);
  test::Require(engine.Expire("b7p", 1, now_us), "expire succeeds");
  test::Require(common::SlotForKey("b7p") == 0, "test key is in first slot");

  expire::ExpireSweeper sweeper(&engine, 8);
  std::size_t deleted = sweeper.SweepOnce(now_us + 2'000'000);
  test::Require(deleted == 1, "one expired key deleted");
  test::Require(engine.GetString("b7p", now_us + 2'000'000).status ==
                    cache::Status::kNotFound,
                "expired key is gone");
}
