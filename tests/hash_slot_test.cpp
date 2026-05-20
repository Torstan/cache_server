#include "test_harness.h"

#include "cache/hash_slot.h"

CACHE_TEST(HashSlotPublishesWriteAndBinlogAtomically) {
  cache::HashSlot slot;
  const std::uint64_t now_us = 1000;

  cache::WriteResult result = slot.SetString("key", "value", now_us);
  test::Require(result.changed, "SET changes slot");
  test::Require(result.seq == 1, "first write seq is 1");

  auto read = slot.GetString("key", now_us);
  test::Require(read.status == cache::Status::kOk, "string key exists");
  test::RequireEqual(read.value, "value", "string value");

  auto logs = slot.CopyLogsAfter(0, 10);
  test::Require(logs.size() == 1, "one log exists");
  test::Require(logs[0].seq == 1, "log seq matches published seq");

  slot.AckLogsThrough(1);
  test::Require(slot.CopyLogsAfter(0, 10).empty(), "acked log is removed");
}
