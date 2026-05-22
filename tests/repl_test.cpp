#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"
#include "common/hash.h"
#include "redis/resp.h"
#include "repl/master_replicator.h"
#include "repl/repl_frame.h"
#include "repl/slave_replicator.h"

namespace {

cache::BinlogRecord MakeRecord(std::uint64_t seq, cache::BinlogOp op,
                               std::vector<std::string> args) {
  cache::BinlogRecord record;
  record.seq = seq;
  record.op = op;
  record.args = std::move(args);
  return record;
}

void WriteString(cache::CacheEngine& engine, std::string_view key,
                 std::string_view value, std::uint64_t now_us) {
  cache::BinlogRecord record;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", std::string(key), std::string(value)};
  engine.Set(key, cache::RedisObject::MakeString(value), std::move(record),
             now_us);
}

void RequireString(cache::CacheEngine& engine, std::string_view key,
                   std::uint64_t now_us, std::string_view expected) {
  auto obj = engine.Get(key, now_us);
  test::Require(obj.has_value(), "string key exists");
  test::Require(obj->Type() == cache::RedisObjectType::kString,
                "object is string");
  const cache::PackedString* value = obj->StringValue();
  test::Require(value != nullptr, "string value pointer exists");
  test::RequireEqual(value->ToString(), expected, "string value");
}

}  // namespace

CACHE_TEST(ReplFrameRoundTripsLog) {
  cache::BinlogRecord record;
  record.seq = 7;
  record.op = cache::BinlogOp::kSet;
  record.args = {"SET", "k", "v"};

  repl::Frame frame = repl::Frame::Log(3, record);
  std::string wire = repl::EncodeFrame(frame);
  auto decoded = repl::DecodeFrame(wire);

  test::Require(decoded.has_value(), "frame decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kLog, "decoded LOG");
  test::Require(decoded->slot_id == 3, "slot id round trips");
  test::Require(decoded->record.seq == 7, "seq round trips");
}

CACHE_TEST(MasterReplicatorUsesAckToCleanLogs) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  WriteString(engine, "k", "v2", 200);

  repl::MasterReplicator repl(&engine);
  repl.OnAck(common::SlotForKey("k"), 2);

  auto records = engine.SlotForKey("k").CopyLogsAfter(0, 10);
  test::Require(records.empty(), "acked logs are cleaned");
}

CACHE_TEST(ReplFrameRejectsMalformedAckCount) {
  std::string wire;
  redis::PackArrayHeader(3, &wire);
  redis::PackBulkString("CACHE.REPL", &wire);
  redis::PackBulkString("ACK", &wire);
  redis::PackBulkString("999999999999999999", &wire);

  test::Require(!repl::DecodeFrame(wire).has_value(),
                "malformed ACK count is rejected");
}

CACHE_TEST(SlaveApplyHoldsOutOfOrderLogsUntilGapFilled) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord seq2 =
      MakeRecord(2, cache::BinlogOp::kSet, {"SET", "k", "v2"});
  cache::BinlogRecord seq1 =
      MakeRecord(1, cache::BinlogOp::kSet, {"SET", "k", "v1"});

  const std::size_t slot = common::SlotForKey("k");
  test::Require(slave.WorkerForSlotForTest(slot) == slot % 4,
                "slot is routed to deterministic apply worker");
  slave.ApplyLogForTest(slot, seq2, 1000);
  test::Require(!engine.Get("k", 1000).has_value(), "seq2 waits for seq1");

  slave.ApplyLogForTest(slot, seq1, 1000);
  RequireString(engine, "k", 1000, "v2");
  test::Require(slave.AppliedSeqForTest(slot) == 2, "applied seq advances");
}

CACHE_TEST(SlaveApplyDoesNotAdvanceSeqForMalformedLog) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord malformed =
      MakeRecord(1, cache::BinlogOp::kSet, {"SET", "k"});

  const std::size_t slot = common::SlotForKey("k");
  slave.ApplyLogForTest(slot, malformed, 1000);

  test::Require(slave.AppliedSeqForTest(slot) == 0,
                "malformed log does not advance seq");
  test::Require(!engine.Get("k", 1000).has_value(),
                "malformed log does not mutate data");
}

CACHE_TEST(SlaveApplySaturatedExpireDoesNotDeleteKey) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);

  cache::BinlogRecord set =
      MakeRecord(1, cache::BinlogOp::kSet, {"SET", "ttl", "v"});

  cache::BinlogRecord expire;
  expire.seq = 2;
  expire.op = cache::BinlogOp::kExpire;
  expire.args = {"EXPIRE", "ttl", "1"};
  expire.remaining_ttl_us = std::numeric_limits<std::uint64_t>::max();

  const std::size_t slot = common::SlotForKey("ttl");
  slave.ApplyLogForTest(slot, set, 1000);
  slave.ApplyLogForTest(slot, expire, 1000);

  RequireString(engine, "ttl", 1000, "v");
  test::Require(slave.AppliedSeqForTest(slot) == 2,
                "saturated expire advances seq");
}

CACHE_TEST(SlaveApplyReplaysGenericCommandTypes) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine, 4);
  const std::uint64_t now_us = 10'000;
  std::map<std::size_t, std::uint64_t> next_seq_by_slot;

  auto apply = [&](cache::BinlogOp op, std::vector<std::string> args) {
    const std::size_t slot = common::SlotForKey(args[1]);
    const std::uint64_t seq = ++next_seq_by_slot[slot];
    cache::BinlogRecord record = MakeRecord(seq, op, std::move(args));
    slave.ApplyLogForTest(slot, record, now_us);
    test::Require(slave.AppliedSeqForTest(slot) == record.seq,
                  "applied seq advances for record");
  };

  apply(cache::BinlogOp::kHSet, {"HSET", "h", "f", "v"});
  auto hash = engine.Get("h", now_us);
  test::Require(hash.has_value(), "hash key exists");
  test::Require(hash->Type() == cache::RedisObjectType::kHash,
                "hash type replays");
  const cache::HashValue* hash_map = hash->Hash();
  test::Require(hash_map != nullptr, "hash pointer exists");
  const cache::PackedString* hash_value =
      hash_map->Find(cache::PackedString("f"));
  test::Require(hash_value != nullptr, "hash field exists");
  test::RequireEqual(hash_value->ToString(), "v", "hash field value");

  apply(cache::BinlogOp::kSAdd, {"SADD", "s", "m"});
  auto set = engine.Get("s", now_us);
  test::Require(set.has_value(), "set key exists");
  test::Require(set->Type() == cache::RedisObjectType::kSet,
                "set type replays");
  const cache::SetValue* set_value = set->Set();
  test::Require(set_value != nullptr, "set pointer exists");
  test::Require(set_value->Contains(cache::PackedString("m")),
                "set member exists");

  apply(cache::BinlogOp::kZAdd, {"ZADD", "z", "1.5", "m"});
  auto zset = engine.Get("z", now_us);
  test::Require(zset.has_value(), "zset key exists");
  test::Require(zset->Type() == cache::RedisObjectType::kZSet,
                "zset type replays");
  const cache::ZSetValue* zset_value = zset->ZSet();
  test::Require(zset_value != nullptr, "zset pointer exists");
  const double* score = zset_value->Find(cache::PackedString("m"));
  test::Require(score != nullptr && *score == 1.5, "zset score replays");

  apply(cache::BinlogOp::kSet, {"SET", "gone", "v"});
  apply(cache::BinlogOp::kDel, {"DEL", "gone"});
  test::Require(!engine.Get("gone", now_us).has_value(), "DEL replays");
}
