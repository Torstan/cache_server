#include "test_harness.h"

#include <string>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "cache/hash_slot.h"
#include "common/hash.h"

CACHE_TEST(Fnva64MatchesKnownVector) {
  test::Require(common::Fnva64("hello") == 0xa430d84680aabd0bULL,
                "FNV-1a hash vector for hello");
}

CACHE_TEST(HashSlotPublishesWriteAndBinlogAtomically) {
  cache::HashSlot slot;
  const std::uint64_t now_us = 1000;

  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", "key", "value"};

  cache::WriteResult result = slot.Set(
      "key", cache::RedisObject::MakeString("value"), std::move(record),
      now_us);
  test::Require(result.changed, "SET changes slot");
  test::Require(result.seq == 1, "first write seq is 1");

  auto read = slot.Get("key", now_us);
  test::Require(read.has_value(), "string key exists");
  const cache::PackedString* value = read->StringValue();
  test::Require(value != nullptr, "string value exists");
  test::RequireEqual(value->ToString(), std::string("value"), "string value");

  auto logs = slot.CopyLogsAfter(0, 10);
  test::Require(logs.size() == 1, "one log exists");
  test::Require(logs[0].seq == 1, "log seq matches published seq");
  test::Require(slot.CopyLogsAfter(0, 0).empty(), "zero log limit returns none");

  slot.AckLogsThrough(1);
  test::Require(slot.CopyLogsAfter(0, 10).empty(), "acked log is removed");
}

CACHE_TEST(HashSlotInstallsReplicaSnapshotAndAdvancesSeq) {
  cache::HashSlot slot;
  const std::uint64_t now_us = 1000;

  cache::BinlogRecord initial;
  initial.args = {"SET", "old", "value"};
  slot.Set("old", cache::RedisObject::MakeString("value"), std::move(initial),
           now_us);

  cache::ObjectMap snapshot_map;
  snapshot_map =
      snapshot_map.Set(cache::PackedString("fresh"),
                       cache::RedisObject::MakeString("snapshot-value"));

  slot.InstallReplicaSnapshot(std::move(snapshot_map), 42);

  test::Require(!slot.Get("old", now_us).has_value(),
                "old value is replaced by snapshot");
  auto fresh = slot.Get("fresh", now_us);
  test::Require(fresh.has_value(), "snapshot value exists");
  const cache::PackedString* value = fresh->StringValue();
  test::Require(value != nullptr, "snapshot string exists");
  test::RequireEqual(value->ToString(), "snapshot-value", "snapshot value");
  test::Require(slot.Snapshot().published_seq == 42,
                "snapshot publishes base sequence");
  test::Require(slot.CopyLogsAfter(0, 10).empty(),
                "installing replica snapshot clears local binlog");

  slot.MarkReplicaAppliedSeq(45);
  test::Require(slot.Snapshot().published_seq == 45,
                "replica applied seq advances published seq");
  slot.MarkReplicaAppliedSeq(44);
  test::Require(slot.Snapshot().published_seq == 45,
                "replica applied seq never moves backwards");
}

CACHE_TEST(CacheEngineInstallsReplicaSnapshotBySlot) {
  cache::CacheEngine engine;
  const std::size_t slot_id = common::SlotForKey("fresh");
  cache::ObjectMap snapshot_map;
  snapshot_map =
      snapshot_map.Set(cache::PackedString("fresh"),
                       cache::RedisObject::MakeString("from-engine"));

  engine.InstallSlotReplicaSnapshot(slot_id, std::move(snapshot_map), 9);

  auto fresh = engine.Get("fresh", 1000);
  test::Require(fresh.has_value(), "engine snapshot value exists");
  test::RequireEqual(fresh->StringValue()->ToString(), "from-engine",
                     "engine snapshot value");
  test::Require(engine.SlotById(slot_id).Snapshot().published_seq == 9,
                "engine publishes snapshot seq");
}

CACHE_TEST(BinlogBufferTracksBytesAndTrimBoundaries) {
  cache::BinlogBuffer buffer;

  cache::BinlogRecord first;
  first.seq = 1;
  first.args = {"SET", "k", "v"};
  const std::size_t first_bytes = cache::EstimateBinlogRecordBytes(first);
  buffer.Append(first);

  cache::BinlogRecord second;
  second.seq = 2;
  second.args = {"APPEND", "k", "tail"};
  const std::size_t second_bytes = cache::EstimateBinlogRecordBytes(second);
  buffer.Append(second);

  test::Require(buffer.RetainedBytes() == first_bytes + second_bytes,
                "binlog retained bytes include both records");
  test::Require(buffer.MinSeq() == 1, "min seq tracks first record");
  test::Require(buffer.MaxSeq() == 2, "max seq tracks last record");

  const std::size_t removed = buffer.AckThroughAndCountBytes(1);
  test::Require(removed == first_bytes, "trim returns removed bytes");
  test::Require(buffer.RetainedBytes() == second_bytes,
                "trim subtracts retained bytes");

  buffer.Clear();
  test::Require(buffer.RetainedBytes() == 0, "clear resets retained bytes");
  test::Require(buffer.MinSeq() == 0, "clear resets min seq");
  test::Require(buffer.MaxSeq() == 0, "clear resets max seq");
}

CACHE_TEST(HashSlotExposesRetainedLogStats) {
  cache::HashSlot slot;
  cache::BinlogRecord record;
  record.args = {"SET", "k", "v"};

  slot.Set("k", cache::RedisObject::MakeString("v"), std::move(record), 1000);

  test::Require(slot.RetainedLogBytes() > 0, "slot reports retained bytes");
  test::Require(slot.MinRetainedLogSeq() == 1, "slot reports min retained seq");
  test::Require(slot.MaxRetainedLogSeq() == 1, "slot reports max retained seq");

  const std::size_t removed = slot.AckLogsThroughAndCountBytes(1);
  test::Require(removed > 0, "slot trim reports removed bytes");
  test::Require(slot.RetainedLogBytes() == 0, "slot retained bytes reset");
}
