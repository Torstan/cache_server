#include "test_harness.h"
#include "replication_test_access.h"

#include <array>
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
#include "command/command_dispatcher.h"
#include "common/hash.h"
#include "redis/resp.h"
#include "repl/master_replicator.h"
#include "repl/repl_frame.h"
#include "repl/replication_link.h"
#include "repl/slave_replicator.h"
#include "repl/snapshot_codec.h"

namespace {

cache::BinlogRecord MakeRecord(std::uint64_t seq, std::vector<std::string> args) {
  cache::BinlogRecord record;
  record.seq = seq;
  record.args = std::move(args);
  return record;
}

void WriteString(cache::CacheEngine& engine, std::string_view key,
                 std::string_view value, std::uint64_t now_us) {
  cache::BinlogRecord record;
  record.args = {"SET", std::string(key), std::string(value)};
  engine.Set(key, cache::RedisObject::MakeString(value), std::move(record),
             now_us);
}


std::string FindKeyForSlotAtLeast(std::size_t min_slot) {
  for (std::size_t i = 0;; ++i) {
    std::string key = "slot-key-" + std::to_string(i);
    if (common::SlotForKey(key) >= min_slot) {
      return key;
    }
  }
}

std::vector<std::string> FindKeysForSlotsBelow(std::size_t max_slot) {
  std::vector<std::string> keys(max_slot);
  std::size_t found = 0;
  for (std::size_t i = 0; found < max_slot; ++i) {
    std::string key = "low-slot-key-" + std::to_string(i);
    const std::size_t slot = common::SlotForKey(key);
    if (slot < max_slot && keys[slot].empty()) {
      keys[slot] = std::move(key);
      ++found;
    }
  }
  return keys;
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
  record.args = {"SET", "k", "v"};

  repl::Frame frame = repl::Frame::Log("test-session", 3, record);
  std::string wire = repl::EncodeFrame(frame);
  auto decoded = repl::DecodeFrame(wire);

  test::Require(decoded.has_value(), "frame decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kLog, "decoded LOG");
  test::Require(decoded->slot_id == 3, "slot id round trips");
  test::Require(decoded->record.seq == 7, "seq round trips");
}

CACHE_TEST(BinlogRecordContainsCommandArguments) {
  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"SET", "key", "value"};

  test::RequireEqual(record.args[0], std::string("SET"), "command in args[0]");
  test::Require(record.args.size() == 3, "args complete");
}

CACHE_TEST(ReplFrameEncodesCommandNameDirectly) {
  cache::BinlogRecord record;
  record.seq = 42;
  record.args = {"DEL", "mykey"};

  repl::Frame frame = repl::Frame::Log("test-session", 5, std::move(record));
  std::string encoded = repl::EncodeFrame(frame);

  auto decoded = repl::DecodeFrame(encoded);
  test::Require(decoded.has_value(), "frame decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kLog, "is LOG frame");
  test::Require(decoded->slot_id == 5, "slot_id preserved");
  test::Require(decoded->record.seq == 42, "seq preserved");
  test::Require(decoded->record.args.size() == 2, "args count");
  test::RequireEqual(decoded->record.args[0], std::string("DEL"),
                     "command name");
  test::RequireEqual(decoded->record.args[1], std::string("mykey"), "key");
}

CACHE_TEST(ReplFrameDecodesUnknownCommandNameDirectly) {
  std::string wire;
  redis::PackArrayHeader(10, &wire);
  redis::PackBulkString("CACHE.REPL", &wire);
  redis::PackBulkString("LOG", &wire);
  redis::PackBulkString("test-session", &wire);
  redis::PackBulkString("5", &wire);
  redis::PackBulkString("42", &wire);
  redis::PackBulkString("1000", &wire);
  redis::PackBulkString("0", &wire);
  redis::PackBulkString("2", &wire);
  redis::PackBulkString("CUSTOM.WRITE", &wire);
  redis::PackBulkString("mykey", &wire);

  auto decoded = repl::DecodeFrame(wire);
  test::Require(decoded.has_value(), "unknown command frame decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kLog, "is LOG frame");
  test::Require(decoded->slot_id == 5, "slot_id preserved");
  test::Require(decoded->record.seq == 42, "seq preserved");
  test::Require(decoded->record.args.size() == 2, "args count");
  test::RequireEqual(decoded->record.args[0], std::string("CUSTOM.WRITE"),
                     "command name");
  test::RequireEqual(decoded->record.args[1], std::string("mykey"), "key");
}

CACHE_TEST(ReplFrameRejectsMismatchedCommandElement) {
  std::string wire;
  redis::PackArrayHeader(11, &wire);
  redis::PackBulkString("CACHE.REPL", &wire);
  redis::PackBulkString("LOG", &wire);
  redis::PackBulkString("test-session", &wire);
  redis::PackBulkString("5", &wire);
  redis::PackBulkString("42", &wire);
  redis::PackBulkString("DEL", &wire);
  redis::PackBulkString("0", &wire);
  redis::PackBulkString("3", &wire);
  redis::PackBulkString("SET", &wire);
  redis::PackBulkString("mykey", &wire);
  redis::PackBulkString("myvalue", &wire);

  test::Require(!repl::DecodeFrame(wire).has_value(),
                "mismatched command element is rejected");
}

CACHE_TEST(ReplFrameRoundTripsEmptyArgsLog) {
  cache::BinlogRecord record;
  record.seq = 42;
  record.args = {};

  repl::Frame frame = repl::Frame::Log("test-session", 5, std::move(record));
  std::string wire = repl::EncodeFrame(frame);
  auto decoded = repl::DecodeFrame(wire);

  test::Require(decoded.has_value(), "empty-args frame decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kLog, "is LOG frame");
  test::Require(decoded->slot_id == 5, "slot_id preserved");
  test::Require(decoded->record.seq == 42, "seq preserved");
  test::Require(decoded->record.args.empty(), "args remain empty");

  std::array<redis::RespValue, 16> scratch{};
  redis::RespResult parsed =
      redis::UnpackOne(wire, scratch.data(), scratch.size(), {});
  test::Require(parsed.status == redis::RespStatus::kOk,
                "encoded frame parses");
  test::Require(parsed.value != nullptr, "encoded frame has value");
  test::Require(parsed.value->type == redis::RespType::kArray,
                "encoded frame array");
  test::Require(parsed.value->element_count == 8,
                "empty args log element count");
  test::Require(parsed.value->elements[5].type == redis::RespType::kBulkString,
                "command metadata is bulk string");
  test::Require(parsed.value->elements[5].text == "0",
                "empty args log has empty command metadata");
}

CACHE_TEST(ReplFrameRejectsMalformedAckCount) {
  std::string wire;
  redis::PackArrayHeader(4, &wire);
  redis::PackBulkString("CACHE.REPL", &wire);
  redis::PackBulkString("ACK", &wire);
  redis::PackBulkString("test-session", &wire);
  redis::PackBulkString("999999999999999999", &wire);

  test::Require(!repl::DecodeFrame(wire).has_value(),
                "malformed ACK count is rejected");
}

CACHE_TEST(SlaveRequestsSnapshotForOutOfOrderLogs) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord seq2 =
      MakeRecord(2, {"SET", "k", "v2"});
  cache::BinlogRecord seq1 =
      MakeRecord(1, {"SET", "k", "v1"});

  const std::size_t slot = common::SlotForKey("k");
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, seq2, 1000);
  test::Require(!engine.Get("k", 1000).has_value(), "seq2 waits for seq1");

  repl::ReplicatorTestAccess::ApplyLog(slave, slot, seq1, 1000);
  test::Require(!engine.Get("k", 1000), "gap requires a snapshot before further replay");
  test::Require(slave.Positions()[slot].second == std::numeric_limits<std::uint64_t>::max(),
                "next poll explicitly requests resynchronization");
}

CACHE_TEST(SlaveApplyDoesNotAdvanceSeqForMalformedLog) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord malformed =
      MakeRecord(1, {"SET", "k"});

  const std::size_t slot = common::SlotForKey("k");
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, malformed, 1000);

  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == 0,
                "malformed log does not advance seq");
  test::Require(!engine.Get("k", 1000).has_value(),
                "malformed log does not mutate data");
}

CACHE_TEST(SlaveApplyUsesRecordedCommandName) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord mismatch =
      MakeRecord(1, {"SET", "k", "v"});

  const std::size_t slot = common::SlotForKey("k");
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, mismatch, 1000);

  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == 1,
                "command-name replay advances seq");
  RequireString(engine, "k", 1000, "v");
}

CACHE_TEST(SlaveApplyRejectsReadCommandReplay) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  WriteString(engine, "k", "v", 1000);
  cache::BinlogRecord read = MakeRecord(1, {"GET", "k"});

  const std::size_t slot = common::SlotForKey("k");
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, read, 1000);

  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == 0,
                "read command does not advance seq");
  RequireString(engine, "k", 1000, "v");
}

CACHE_TEST(SlaveApplyAdvancesSeqForSuccessfulNoOpWriteCommand) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  const std::uint64_t now_us = 1000;

  cache::BinlogRecord first =
      MakeRecord(1, {"SADD", "s", "m"});
  cache::BinlogRecord duplicate =
      MakeRecord(2, {"SADD", "s", "m"});

  const std::size_t slot = common::SlotForKey("s");
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, first, now_us);
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, duplicate, now_us);

  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == 2,
                "successful write command advances seq even when response is 0");
  auto obj = engine.Get("s", now_us);
  test::Require(obj.has_value(), "set key exists");
  test::Require(obj->Type() == cache::RedisObjectType::kSet,
                "set type is preserved");
  const cache::SetValue* set = obj->Set();
  test::Require(set != nullptr, "set pointer exists");
  test::Require(set->Contains(cache::PackedString("m")),
                "member remains present");
}

CACHE_TEST(SlaveApplyDecodedDelFrame) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  repl::ReplicatorTestAccess::StartSession(slave, "test-session");
  const std::uint64_t now_us = 1000;

  WriteString(engine, "k", "v", now_us);

  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"DEL", "k"};
  repl::Frame frame = repl::Frame::Log("test-session", common::SlotForKey("k"),
                                       std::move(record));
  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "frame decodes");

  slave.EnqueueFrame(std::move(*decoded));

  test::Require(!engine.Get("k", now_us).has_value(),
                "decoded DEL frame applies");
}

CACHE_TEST(SlaveApplyRejectsDecodedReadFrame) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  repl::ReplicatorTestAccess::StartSession(slave, "test-session");
  const std::uint64_t now_us = 1000;

  WriteString(engine, "k", "v", now_us);

  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"GET", "k"};
  repl::Frame frame = repl::Frame::Log("test-session", common::SlotForKey("k"),
                                       std::move(record));
  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "frame decodes");

  slave.EnqueueFrame(std::move(*decoded));

  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, common::SlotForKey("k")) == 0,
                "decoded read frame does not advance seq");
  RequireString(engine, "k", now_us, "v");
}

CACHE_TEST(SlaveApplyRejectsFrameSlotKeyMismatch) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  repl::ReplicatorTestAccess::StartSession(slave, "test-session");
  const std::uint64_t now_us = 1000;
  const std::string key = "slot_mismatch_key";
  const std::size_t key_slot = common::SlotForKey(key);
  const std::size_t frame_slot = key_slot == 0 ? 1 : 0;

  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"SET", key, "v"};
  repl::Frame frame = repl::Frame::Log("test-session", frame_slot,
                                       std::move(record));
  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "frame decodes");

  slave.EnqueueFrame(std::move(*decoded));

  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, frame_slot) == 0,
                "mismatched frame slot does not advance");
  test::Require(!engine.Get(key, now_us).has_value(),
                "mismatched frame slot does not mutate key");
}

CACHE_TEST(ReplEndToEndCommandAgnostic) {
  cache::CacheEngine master_engine;
  cache::CacheEngine slave_engine;
  repl::SlaveReplicator replicator(&slave_engine);
  repl::ReplicatorTestAccess::StartSession(replicator, "test-session");
  const std::uint64_t now_us = 1000000;

  cache::BinlogRecord set_rec;
  set_rec.args = {"SET", "key1", "value1"};
  master_engine.Set("key1", cache::RedisObject::MakeString("value1"),
                    std::move(set_rec), now_us);

  auto logs = master_engine.SlotForKey("key1").CopyLogsAfter(0, 10);
  test::Require(logs.size() == 1, "one log entry");

  repl::Frame frame =
      repl::Frame::Log("test-session", common::SlotForKey("key1"),
                       std::move(logs[0]));
  std::string wire = repl::EncodeFrame(frame);
  auto decoded = repl::DecodeFrame(wire);
  test::Require(decoded.has_value(), "frame decodes");
  test::Require(decoded->record.args.size() == 3, "decoded args count");
  test::RequireEqual(decoded->record.args[0], std::string("SET"),
                     "decoded command name");

  replicator.EnqueueFrame(std::move(*decoded));

  auto result = slave_engine.Get("key1", now_us);
  test::Require(result.has_value(), "key replicated");
  test::Require(result->Type() == cache::RedisObjectType::kString,
                "type preserved");
  const cache::PackedString* value = result->StringValue();
  test::Require(value != nullptr, "string value pointer exists");
  test::RequireEqual(value->ToString(), std::string("value1"),
                     "value preserved");
}

CACHE_TEST(SlaveApplyViaCommandDispatcher) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"SET", "key1", "value1"};

  test::Require(repl::ReplicatorTestAccess::ApplyRecord(slave, record, 1000000),
                "dispatcher replay succeeds");

  auto result = engine.Get("key1", 1000000);
  test::Require(result.has_value(), "key exists");
  test::Require(result->Type() == cache::RedisObjectType::kString,
                "is string");
  const cache::PackedString* value = result->StringValue();
  test::RequireEqual(value->ToString(), std::string("value1"),
                     "value matches");
}

CACHE_TEST(SlaveApplyUsesAbsoluteDeadline) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord set =
      MakeRecord(1, {"SET", "ttl", "v"});

  cache::BinlogRecord expire;
  expire.seq = 2;
  expire.args = {"EXPIRE", "ttl", "1"};
  expire.deadline_us = std::numeric_limits<std::uint64_t>::max();
  expire.written_at_us = 1000;

  const std::uint64_t now_us = 1000;
  test::Require(repl::ReplicatorTestAccess::ApplyRecord(slave, set, now_us),
                "dispatcher SET replay succeeds");
  test::Require(repl::ReplicatorTestAccess::ApplyRecord(slave, expire, now_us),
                "dispatcher EXPIRE replay succeeds");

  RequireString(engine, "ttl", now_us + 2'000'000, "v");
}

CACHE_TEST(SlaveApplyViaCommandDispatcherRejectsUnknownCommand) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord record =
      MakeRecord(1, {"NO_SUCH_COMMAND", "k", "v"});

  test::Require(!repl::ReplicatorTestAccess::ApplyRecord(slave, record, 1000),
                "dispatcher errors are rejected");
  test::Require(!engine.Get("k", 1000).has_value(),
                "unknown command does not mutate data");
}

CACHE_TEST(SlaveApplyAllCommandTypesViaDispatcher) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  const std::uint64_t now_us = 1000000;
  std::map<std::size_t, std::uint64_t> next_seq;

  auto apply = [&](std::vector<std::string> args,
                   std::uint64_t ttl_us = 0) {
    const std::size_t slot = common::SlotForKey(args[1]);
    cache::BinlogRecord rec;
    rec.seq = ++next_seq[slot];
    rec.args = std::move(args);
    rec.written_at_us = now_us;
    rec.deadline_us = ttl_us ? now_us + ttl_us : 0;
    repl::ReplicatorTestAccess::ApplyLog(slave, slot, rec, now_us);
  };

  // SET
  apply({"SET", "str_key", "str_val"});
  auto str_obj = engine.Get("str_key", now_us);
  test::Require(str_obj.has_value(), "SET creates key");
  test::Require(str_obj->Type() == cache::RedisObjectType::kString,
                "SET creates string");

  // HSET
  apply({"HSET", "hash_key", "field1", "val1"});
  auto hash_obj = engine.Get("hash_key", now_us);
  test::Require(hash_obj.has_value(), "HSET creates key");
  test::Require(hash_obj->Type() == cache::RedisObjectType::kHash,
                "HSET creates hash");

  // SADD
  apply({"SADD", "set_key", "member1"});
  auto set_obj = engine.Get("set_key", now_us);
  test::Require(set_obj.has_value(), "SADD creates key");
  test::Require(set_obj->Type() == cache::RedisObjectType::kSet,
                "SADD creates set");

  // ZADD
  apply({"ZADD", "zset_key", "1.5", "member1"});
  auto zset_obj = engine.Get("zset_key", now_us);
  test::Require(zset_obj.has_value(), "ZADD creates key");
  test::Require(zset_obj->Type() == cache::RedisObjectType::kZSet,
                "ZADD creates zset");

  // DEL
  apply({"DEL", "str_key"});
  test::Require(!engine.Get("str_key", now_us).has_value(), "DEL removes key");

  // EXPIRE
  apply({"EXPIRE", "hash_key", "300"}, 300000000);
  auto ttl = engine.Ttl("hash_key", now_us);
  test::Require(ttl > 0, "EXPIRE sets TTL");
}

CACHE_TEST(SlaveApplySaturatedExpireDoesNotDeleteKey) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);

  cache::BinlogRecord set =
      MakeRecord(1, {"SET", "ttl", "v"});

  cache::BinlogRecord expire;
  expire.seq = 2;
  expire.args = {"EXPIRE", "ttl", "1"};
  expire.deadline_us = std::numeric_limits<std::uint64_t>::max();
  expire.written_at_us = 1000;

  const std::size_t slot = common::SlotForKey("ttl");
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, set, 1000);
  repl::ReplicatorTestAccess::ApplyLog(slave, slot, expire, 1000);

  RequireString(engine, "ttl", 1000, "v");
  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == 2,
                "saturated expire advances seq");
}

CACHE_TEST(SlaveApplyReplaysGenericCommandTypes) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  const std::uint64_t now_us = 10'000;
  std::map<std::size_t, std::uint64_t> next_seq_by_slot;

  auto apply = [&](std::vector<std::string> args) {
    const std::size_t slot = common::SlotForKey(args[1]);
    const std::uint64_t seq = ++next_seq_by_slot[slot];
    cache::BinlogRecord record = MakeRecord(seq, std::move(args));
    repl::ReplicatorTestAccess::ApplyLog(slave, slot, record, now_us);
    test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == record.seq,
                  "applied seq advances for record");
  };

  apply({"HSET", "h", "f", "v"});
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

  apply({"SADD", "s", "m"});
  auto set = engine.Get("s", now_us);
  test::Require(set.has_value(), "set key exists");
  test::Require(set->Type() == cache::RedisObjectType::kSet,
                "set type replays");
  const cache::SetValue* set_value = set->Set();
  test::Require(set_value != nullptr, "set pointer exists");
  test::Require(set_value->Contains(cache::PackedString("m")),
                "set member exists");

  apply({"ZADD", "z", "1.5", "m"});
  auto zset = engine.Get("z", now_us);
  test::Require(zset.has_value(), "zset key exists");
  test::Require(zset->Type() == cache::RedisObjectType::kZSet,
                "zset type replays");
  const cache::ZSetValue* zset_value = zset->ZSet();
  test::Require(zset_value != nullptr, "zset pointer exists");
  const double* score = zset_value->Find(cache::PackedString("m"));
  test::Require(score != nullptr && *score == 1.5, "zset score replays");

  apply({"SET", "gone", "v"});
  apply({"DEL", "gone"});
  test::Require(!engine.Get("gone", now_us).has_value(), "DEL replays");
}

CACHE_TEST(ReplFrameRoundTripsHelloWithSlotPositions) {
  repl::Frame frame = repl::Frame::Hello(
      "replica-a", repl::kProtocolVersion, "old-session", {{3, 7}, {5, 9}});

  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "HELLO decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kHello, "is HELLO");
  test::RequireEqual(decoded->replica_id, "replica-a", "replica id");
  test::Require(decoded->proto_version == repl::kProtocolVersion, "protocol version");
  test::RequireEqual(decoded->session_id, "old-session", "previous session");
  test::Require(decoded->slot_positions.size() == 2, "slot count");
  test::Require(decoded->slot_positions[0].first == 3, "first slot id");
  test::Require(decoded->slot_positions[0].second == 7, "first seq");
}

CACHE_TEST(ReplFrameRoundTripsSnapshotWithSession) {
  repl::Frame frame = repl::Frame::Snapshot("session-1", 11, 42, "payload");

  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "SNAPSHOT decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kSnapshot, "is SNAPSHOT");
  test::RequireEqual(decoded->session_id, "session-1", "session id");
  test::Require(decoded->slot_id == 11, "slot id");
  test::Require(decoded->base_seq == 42, "base seq");
  test::RequireEqual(decoded->snapshot_payload, "payload", "payload");
}

CACHE_TEST(ReplFrameRoundTripsLogWithSession) {
  cache::BinlogRecord record;
  record.seq = 8;
  record.args = {"SET", "k", "v"};

  repl::Frame frame = repl::Frame::Log("session-2", 4, std::move(record));

  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "LOG decodes");
  test::RequireEqual(decoded->session_id, "session-2", "session id");
  test::Require(decoded->slot_id == 4, "slot id");
  test::Require(decoded->record.seq == 8, "seq");
  test::RequireEqual(decoded->record.args[0], "SET", "command");
}

CACHE_TEST(ReplFrameRoundTripsCompletionWithSession) {
  repl::Frame frame = repl::Frame::Done("session-3", {{1, 2}, {2, 5}});

  auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
  test::Require(decoded.has_value(), "ACK decodes");
  test::Require(decoded->subcmd == repl::Subcmd::kDone, "is ACK");
  test::RequireEqual(decoded->session_id, "session-3", "session id");
  test::Require(decoded->slot_positions.size() == 2, "acked slots");
}

CACHE_TEST(SlaveAppliesSnapshotAtomicallyForCurrentSession) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  repl::ReplicatorTestAccess::StartSession(slave, "session-1");

  WriteString(engine, "old", "value", 1000);

  cache::ObjectMap map;
  map = map.Set(cache::PackedString("fresh"),
                cache::RedisObject::MakeString("snapshot"));
  std::string payload = repl::EncodeSnapshotPayload(map);
  repl::Frame snapshot = repl::Frame::Snapshot(
      "session-1", common::SlotForKey("fresh"), 12, payload);

  slave.EnqueueFrame(std::move(snapshot));

  RequireString(engine, "fresh", 1000, "snapshot");
  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, common::SlotForKey("fresh")) == 12,
                "snapshot applies base seq");
  test::Require(repl::ReplicatorTestAccess::State(slave, common::SlotForKey("fresh")) ==
                    repl::SlaveReplicator::SlotState::kCatchingUp,
                "snapshot waits for batch completion");
}

CACHE_TEST(SlaveRejectsOldSessionFrames) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  repl::ReplicatorTestAccess::StartSession(slave, "current");

  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"SET", "k", "v"};
  slave.EnqueueFrame(repl::Frame::Log("old", common::SlotForKey("k"),
                                      std::move(record)));

  test::Require(!engine.Get("k", 1000).has_value(),
                "old session log ignored");
  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, common::SlotForKey("k")) == 0,
                "old session does not advance seq");
}

CACHE_TEST(SlaveKeepsSlotOfflineAfterBadSnapshotPayload) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  const std::size_t slot = common::SlotForKey("k");
  repl::ReplicatorTestAccess::StartSession(slave, "session-1");

  slave.EnqueueFrame(repl::Frame::Snapshot("session-1", slot, 3, "bad"));

  test::Require(repl::ReplicatorTestAccess::State(slave, slot) ==
                    repl::SlaveReplicator::SlotState::kOffline,
                "bad snapshot leaves slot offline");
  test::Require(repl::ReplicatorTestAccess::AppliedSeq(slave, slot) == 0,
                "bad snapshot does not ack");
}

CACHE_TEST(MasterHelloStreamsLogsWhenBacklogAvailable) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  WriteString(engine, "k", "v2", 200);

  repl::MasterReplicator master(&engine);
  auto previous = repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello(
      "replica-a", repl::kProtocolVersion, "", {{common::SlotForKey("k"), 0}}));
  repl::Frame hello = repl::Frame::Hello(
      "replica-a", repl::kProtocolVersion, previous, {{common::SlotForKey("k"), 1}});
  std::string session = repl::ReplicatorTestAccess::Hello(master, hello);

  auto frames = repl::ReplicatorTestAccess::Frames(master, "replica-a", 16);
  test::Require(!frames.empty(), "master emits frames");
  test::Require(frames[0].subcmd == repl::Subcmd::kLog,
                "available backlog streams LOG");
  test::RequireEqual(frames[0].session_id, session, "session id attached");
  test::Require(frames[0].record.seq == 2, "streams next log");
}

CACHE_TEST(MasterStreamsMultipleLogsForSlotWithinBudget) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  WriteString(engine, "k", "v2", 200);
  WriteString(engine, "k", "v3", 300);

  repl::MasterReplicator master(&engine);
  const std::size_t slot = common::SlotForKey("k");
  repl::Frame hello = repl::Frame::Hello("replica-a", 1, "", {{slot, 0}});
  (void)repl::ReplicatorTestAccess::Hello(master, hello);

  auto frames = repl::ReplicatorTestAccess::Frames(master, "replica-a", 16);
  test::Require(frames.size() == 3,
                "streams consecutive logs for one slot in one poll");
  for (const auto& frame : frames) {
    test::Require(frame.subcmd == repl::Subcmd::kLog, "streams LOG frames");
    test::Require(frame.slot_id == slot, "streams one slot");
  }
  test::Require(frames[0].record.seq == 1, "streams first log");
  test::Require(frames[1].record.seq == 2, "streams second log");
  test::Require(frames[2].record.seq == 3, "streams third log");
}

CACHE_TEST(MasterSlotCountBudgetReachesHighSlotLog) {
  cache::CacheEngine engine;
  const std::size_t small_budget = 1024;
  std::vector<std::string> low_slot_keys = FindKeysForSlotsBelow(small_budget);
  std::vector<std::pair<std::size_t, std::uint64_t>> positions;
  positions.reserve(small_budget + 1);
  for (const std::string& key : low_slot_keys) {
    WriteString(engine, key, "v1", 100);
    positions.push_back({common::SlotForKey(key), 0});
  }
  const std::string high_slot_key = FindKeyForSlotAtLeast(small_budget);
  const std::size_t high_slot = common::SlotForKey(high_slot_key);
  WriteString(engine, high_slot_key, "v1", 100);
  positions.push_back({high_slot, 0});

  repl::MasterReplicator master(&engine);
  repl::Frame hello = repl::Frame::Hello("replica-a", 1, "", positions);
  (void)repl::ReplicatorTestAccess::Hello(master, hello);

  auto small_budget_frames =
      repl::ReplicatorTestAccess::Frames(master, "replica-a", small_budget);
  test::Require(small_budget_frames.size() == small_budget,
                "small budget is consumed before high slot");
  for (const auto& frame : small_budget_frames) {
    test::Require(frame.slot_id != high_slot,
                  "fixed 1024 frame budget cannot reach this high slot");
  }

  auto slot_budget_frames =
      repl::ReplicatorTestAccess::Frames(master, "replica-a", engine.SlotCount());
  test::Require(slot_budget_frames.size() == 1,
                "slot-count frame budget reaches high slot log");
  test::Require(slot_budget_frames[0].subcmd == repl::Subcmd::kLog,
                "high slot emits LOG");
  test::Require(slot_budget_frames[0].slot_id == high_slot,
                "emits the high slot log");
  test::Require(slot_budget_frames[0].record.seq == 1,
                "emits the pending high slot seq");
}

CACHE_TEST(MasterHelloSnapshotsWhenBacklogMissing) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  const std::size_t slot = common::SlotForKey("k");
  engine.SlotById(slot).AckLogsThrough(1);

  repl::MasterReplicator master(&engine);
  repl::Frame hello = repl::Frame::Hello("replica-a", 1, "", {{slot, 0}});
  std::string session = repl::ReplicatorTestAccess::Hello(master, hello);

  auto frames = repl::ReplicatorTestAccess::Frames(master, "replica-a", 16);
  test::Require(!frames.empty(), "master emits frames");
  test::Require(frames[0].subcmd == repl::Subcmd::kSnapshot,
                "missing backlog sends snapshot");
  test::RequireEqual(frames[0].session_id, session, "session id attached");
  test::Require(frames[0].slot_id == slot, "snapshot slot");
}

CACHE_TEST(MasterAckCleanupUsesSlowestReplica) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  WriteString(engine, "k", "v2", 200);
  const std::size_t slot = common::SlotForKey("k");

  repl::MasterReplicator master(&engine);
  const std::string fast =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("fast", 1, "", {{slot, 0}}));
  const std::string slow =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("slow", 1, "", {{slot, 0}}));

  master.Poll(repl::Frame::Hello("fast", repl::kProtocolVersion, fast, {{slot, 2}}), 16);
  master.Maintain();
  test::Require(!engine.SlotById(slot).CopyLogsAfter(0, 10).empty(),
                "slow replica keeps logs retained");

  master.Poll(repl::Frame::Hello("slow", repl::kProtocolVersion, slow, {{slot, 2}}), 16);
  master.Maintain();
  test::Require(engine.SlotById(slot).CopyLogsAfter(0, 10).empty(),
                "all replicas acked logs are removed");
}

CACHE_TEST(MasterBudgetPressureMarksLaggingSlotForSnapshot) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);
  WriteString(engine, "k", "v2", 200);
  const std::size_t slot = common::SlotForKey("k");

  repl::MasterReplicator master(&engine);
  master.SetGlobalBinlogBudget(1);
  repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("lagging", 1, "", {{slot, 0}}));
  master.Maintain();

  auto frames = repl::ReplicatorTestAccess::Frames(master, "lagging", 16);
  test::Require(!frames.empty(), "budget pressure emits frame");
  test::Require(frames[0].subcmd == repl::Subcmd::kSnapshot,
                "lagging slot resyncs by snapshot");
}

CACHE_TEST(ReplicationLinkBuildsHelloFromSlaveState) {
  cache::CacheEngine engine;
  repl::SlaveReplicator slave(&engine);
  repl::ReplicatorTestAccess::StartSession(slave, "old-session");

  repl::Frame hello =
      repl::BuildHelloFrame("replica-a", "old-session", slave, 1);

  test::Require(hello.subcmd == repl::Subcmd::kHello, "builds HELLO");
  test::RequireEqual(hello.replica_id, "replica-a", "replica id");
  test::RequireEqual(hello.session_id, "old-session", "previous session");
  test::Require(hello.slot_positions.size() == engine.SlotCount(),
                "reports every slot");
}

CACHE_TEST(MasterHelloTreatsInitialEmptyBacklogSlotAsCaughtUp) {
  cache::CacheEngine engine;
  repl::MasterReplicator master(&engine);
  const std::size_t slot = common::SlotForKey("empty-slot-key");

  std::string session =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("replica-a", 1, "", {{slot, 0}}));

  auto frames =
      repl::ReplicatorTestAccess::Frames(master, "replica-a", engine.SlotCount());
  test::Require(!session.empty(), "initial hello creates an epoch-qualified session");
  for (const repl::Frame& frame : frames) {
    test::Require(frame.slot_id != slot,
                  "initially empty slot needs no snapshot or log frame");
  }
}

CACHE_TEST(MasterHelloReusesPreviousSessionForReconnect) {
  cache::CacheEngine engine;
  WriteString(engine, "k", "v1", 100);

  repl::MasterReplicator master(&engine);
  const std::size_t slot = common::SlotForKey("k");
  std::string first_session =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("replica-a", 1, "", {{slot, 0}}));

  std::string resumed_session = repl::ReplicatorTestAccess::Hello(master,
      repl::Frame::Hello("replica-a", 1, first_session, {{slot, 1}}));

  test::RequireEqual(resumed_session, first_session,
                     "matching previous session resumes");
}

CACHE_TEST(MasterStartsNewSessionForUnknownPreviousSession) {
  cache::CacheEngine engine;
  repl::MasterReplicator master(&engine);
  const std::size_t slot = common::SlotForKey("k");

  std::string first_session =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("replica-a", 1, "", {{slot, 0}}));
  std::string new_session = repl::ReplicatorTestAccess::Hello(master,
      repl::Frame::Hello("replica-a", 1, "stale-session", {{slot, 0}}));

  test::Require(new_session != first_session,
                "stale previous session starts a new session");
}

CACHE_TEST(ReplicaConvergesAfterInitialEmptyPoll) {
  cache::CacheEngine master_engine;
  cache::CacheEngine replica_engine;
  repl::MasterReplicator master(&master_engine);
  repl::SlaveReplicator replica(&replica_engine);

  repl::Frame initial_hello =
      repl::BuildHelloFrame("replica-a", replica.CurrentSessionId(), replica, 1);
  (void)repl::ReplicatorTestAccess::Hello(master, initial_hello);
  auto initial_frames =
      repl::ReplicatorTestAccess::Frames(master, "replica-a", master_engine.SlotCount());
  test::Require(initial_frames.empty(), "empty initial poll has no frames");
  test::Require(replica.CurrentSessionId().empty(),
                "replica has no session until it receives a frame");

  WriteString(master_engine, "k4", "100", 1000);
  command::CommandDispatcher dispatcher;
  (void)dispatcher.Execute(std::vector<std::string>{"INCR", "k4"},
                           master_engine, 1000);
  (void)dispatcher.Execute(std::vector<std::string>{"INCRBY", "k4", "10"},
                           master_engine, 1000);

  for (int poll = 0; poll < 3; ++poll) {
    repl::Frame hello = repl::BuildHelloFrame(
        "replica-a", replica.CurrentSessionId(), replica, 1);
    (void)repl::ReplicatorTestAccess::Hello(master, hello);
    for (repl::Frame& frame : repl::ReplicatorTestAccess::Frames(master,
             "replica-a", master_engine.SlotCount())) {
      if (frame.subcmd == repl::Subcmd::kSnapshot ||
          frame.subcmd == repl::Subcmd::kLog) {
        if (replica.CurrentSessionId().empty()) {
          repl::ReplicatorTestAccess::StartSession(replica, frame.session_id);
        }
        replica.EnqueueFrame(std::move(frame));
      }
    }
  }

  RequireString(replica_engine, "k4", 1000, "111");
}

CACHE_TEST(MasterTwoReplicasEventuallyReceiveSameLog) {
  cache::CacheEngine master_engine;
  cache::CacheEngine replica_a_engine;
  cache::CacheEngine replica_b_engine;
  repl::MasterReplicator master(&master_engine);
  repl::SlaveReplicator replica_a(&replica_a_engine);
  repl::SlaveReplicator replica_b(&replica_b_engine);

  const std::size_t slot = common::SlotForKey("k");
  std::string session_a =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("a", 1, "", {{slot, 0}}));
  std::string session_b =
      repl::ReplicatorTestAccess::Hello(master, repl::Frame::Hello("b", 1, "", {{slot, 0}}));
  repl::ReplicatorTestAccess::StartSession(replica_a, session_a);
  repl::ReplicatorTestAccess::StartSession(replica_b, session_b);

  WriteString(master_engine, "k", "v", 1000);

  for (repl::Frame& frame : repl::ReplicatorTestAccess::Frames(master, "a", 16)) {
    replica_a.EnqueueFrame(std::move(frame));
  }
  for (repl::Frame& frame : repl::ReplicatorTestAccess::Frames(master, "b", 16)) {
    replica_b.EnqueueFrame(std::move(frame));
  }

  RequireString(replica_a_engine, "k", 1000, "v");
  RequireString(replica_b_engine, "k", 1000, "v");
}
