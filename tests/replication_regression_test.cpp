#include "test_harness.h"
#include "replication_test_access.h"

#include <set>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "common/hash.h"
#include "repl/master_replicator.h"
#include "repl/slave_replicator.h"
#include "repl/repl_frame.h"

namespace {
protocol::Response Run(command::CommandDispatcher& dispatcher,
                       cache::CacheEngine& engine,
                       std::initializer_list<std::string> args,
                       std::uint64_t now = 1'000'000) {
  return dispatcher.Execute(std::vector<std::string>(args), engine, now);
}

CACHE_TEST(ReplicationWirePreservesPrimaryTimeAndDeadline) {
  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"SET", "k", "v"};
  record.written_at_us = 1'000'000;
  record.deadline_us = 2'000'000;
  auto decoded = repl::DecodeFrame(repl::EncodeFrame(repl::Frame::Log("s", 1, record)));
  test::Require(decoded.has_value(), "log frame decoded");
  test::Require(decoded->record.written_at_us == 1'000'000 &&
                decoded->record.deadline_us == 2'000'000,
                "wire must preserve the primary time domain");
}

void Poll(repl::MasterReplicator& master, repl::SlaveReplicator& replica,
          std::size_t limit = 4096) {
  auto hello = repl::Frame::Hello("replica", repl::kProtocolVersion,
                                 replica.CurrentSessionId(), replica.Positions());
  auto frames = master.Poll(hello, limit);
  test::Require(!frames.empty(), "every poll has a control response");
  for (const auto& frame : frames) {
    auto decoded = repl::DecodeFrame(repl::EncodeFrame(frame));
    test::Require(decoded && replica.EnqueueFrame(std::move(*decoded)),
                  "a complete batch applies through the wire codec");
  }
}
}

CACHE_TEST(SpopLogsTheMembersActuallyRemoved) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  Run(dispatcher, engine, {"SADD", "s", "a", "b", "c"});
  auto removed = Run(dispatcher, engine, {"SPOP", "s"});
  auto logs = engine.SlotForKey("s").CopyLogsAfter(1, 10);
  test::Require(logs.size() == 1, "one committed removal");
  test::Require(logs[0].args == std::vector<std::string>{"SREM", "s", removed.text},
                "replication removes the selected member without another random choice");
}

CACHE_TEST(HdelLastFieldCommitsOnce) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  Run(dispatcher, engine, {"HSET", "h", "f", "v"});
  auto response = Run(dispatcher, engine, {"HDEL", "h", "f"});
  test::Require(response.integer == 1, "one field removed");
  test::Require(!engine.Get("h", 1'000'000), "empty hash deleted");
  test::Require(engine.SlotForKey("h").Snapshot().published_seq == 2,
                "removal and deletion are a single atomic write");
}

CACHE_TEST(DelayedReplicaReplayDoesNotRestartTtl) {
  cache::CacheEngine source, target;
  command::CommandDispatcher dispatcher;
  repl::SlaveReplicator replica(&target);
  Run(dispatcher, source, {"SET", "ttl", "v", "EX", "1"});
  const auto slot = common::SlotForKey("ttl");
  for (const auto& record : source.SlotById(slot).CopyLogsAfter(0, 10)) {
    repl::ReplicatorTestAccess::ApplyLog(replica, slot, record, 3'000'000);
  }
  test::Require(!target.Get("ttl", 3'000'000), "replica must not resurrect expired data");
}

CACHE_TEST(BudgetPressureActuallyReclaimsLogs) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  Run(dispatcher, engine, {"SET", "budget", "value"});
  repl::MasterReplicator master(&engine);
  master.SetGlobalBinlogBudget(1);
  master.Maintain();
  test::Require(engine.SlotForKey("budget").RetainedLogBytes() <= 1,
                "budget must reclaim bytes even with no replicas");
}

CACHE_TEST(EmptyReplicationPollMakesSlotsReadable) {
  cache::CacheEngine source, target;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  auto hello = repl::Frame::Hello("replica", repl::kProtocolVersion, "", {{0, 0}});
  auto session = repl::ReplicatorTestAccess::Hello(master, hello);
  replica.EnqueueFrame(repl::Frame::Begin(session, true));
  for (auto& frame : repl::ReplicatorTestAccess::Frames(master, "replica", 10)) {
    replica.EnqueueFrame(std::move(frame));
  }
  replica.EnqueueFrame(repl::Frame::Done(session, {{0, 0}}));
  test::Require(replica.CanReadSlot(0), "empty slots complete synchronization");
}

CACHE_TEST(ReplicaReadPolicyChecksAllKeysAndActualScanSlots) {
  cache::CacheEngine target;
  repl::SlaveReplicator replica(&target);
  replica.EnqueueFrame(repl::Frame::Begin("s", true));
  cache::BinlogRecord record;
  record.seq = 1;
  record.args = {"SET", "ready", "v"};
  const auto slot = common::SlotForKey("ready");
  replica.EnqueueFrame(repl::Frame::Log("s", slot, record));
  replica.EnqueueFrame(repl::Frame::Done("s", {{slot, 1}}));
  test::Require(replica.ExecuteRead({"GET", "ready"}, 1'000'000, false).type ==
                    protocol::ResponseType::kError, "disabled reads are rejected");
  test::Require(replica.ExecuteRead({"MGET", "ready", "offline"}, 1'000'000, true).type ==
                    protocol::ResponseType::kError, "every MGET slot must be ready");
  test::Require(replica.ExecuteRead({"EXISTS", "ready", "offline"}, 1'000'000, true).type ==
                    protocol::ResponseType::kError, "every EXISTS slot must be ready");
  const auto scan = replica.ExecuteRead({"SCAN", std::to_string(slot), "COUNT", "1"},
                                         1'000'000, true);
  test::Require(scan.type == protocol::ResponseType::kArray &&
                    scan.elements[1].elements.size() == 1,
                "SCAN validates visited slots, not a hash of its cursor");
  test::Require(replica.ExecuteRead({"SET", "ready", "bad"}, 1'000'000, true).type ==
                    protocol::ResponseType::kError, "writes remain disabled");
}

CACHE_TEST(ReplicaResumesAfterBudgetEvictionAndPrimaryReplacement) {
  cache::CacheEngine source, replacement, target;
  command::CommandDispatcher dispatcher;
  repl::MasterReplicator master(&source), next_master(&replacement);
  repl::SlaveReplicator replica(&target);
  Run(dispatcher, source, {"SET", "k", "old"});
  Poll(master, replica);
  test::RequireEqual(replica.ExecuteRead({"GET", "k"}, 1'000'000, true).text, "old", "initial sync");
  Run(dispatcher, source, {"SET", "k", "updated"});
  master.SetGlobalBinlogBudget(0);
  master.Maintain();
  Poll(master, replica);
  test::RequireEqual(replica.ExecuteRead({"GET", "k"}, 1'000'000, true).text, "updated", "missing logs resnapshot");
  Run(dispatcher, replacement, {"SET", "k", "replacement"});
  Poll(next_master, replica);
  test::RequireEqual(replica.ExecuteRead({"GET", "k"}, 1'000'000, true).text, "replacement", "new source epoch resets old positions");
}

CACHE_TEST(DelayedReplayPreservesExpiryExtensionAndDoesNotRelog) {
  cache::CacheEngine source, target;
  command::CommandDispatcher dispatcher;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  Run(dispatcher, source, {"HSET", "h", "a", "1"}, 1'000'000);
  Run(dispatcher, source, {"EXPIRE", "h", "1"}, 1'000'000);
  Run(dispatcher, source, {"HSET", "h", "b", "2"}, 1'500'000);
  Run(dispatcher, source, {"EXPIRE", "h", "100"}, 1'500'000);
  Poll(master, replica);
  auto reply = replica.ExecuteRead({"HLEN", "h"}, 3'000'000, true);
  test::Require(reply.type == protocol::ResponseType::kInteger && reply.integer == 2,
                "replay uses primary write time even when old TTL has elapsed locally");
  test::Require(target.SlotForKey("h").RetainedLogBytes() == 0, "replica never generates local logs");
}

CACHE_TEST(PrimaryExpiryDeletionDoesNotResurrectReplicaValue) {
  cache::CacheEngine source, target;
  command::CommandDispatcher dispatcher;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  Run(dispatcher, source, {"SET", "ttl", "v", "EX", "1"});
  Poll(master, replica);
  test::Require(source.SlotForKey("ttl").DeleteExpired(1, 3'000'000) == 1,
                "primary physically expires the value");
  Poll(master, replica);
  test::Require(replica.ExecuteRead({"GET", "ttl"}, 3'000'000, true).type ==
                    protocol::ResponseType::kNullBulkString,
                "expiry DEL must remove the replica value, not clear its TTL");
  test::Require(target.SlotForKey("ttl").Snapshot().map.Size() == 0,
                "primary expiry also releases replica storage");
}

CACHE_TEST(ReplicationBatchRotatesPastBusySlots) {
  cache::CacheEngine source, target;
  command::CommandDispatcher dispatcher;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  std::string hot = "hot", cold = "cold";
  if (common::SlotForKey(hot) > common::SlotForKey(cold)) std::swap(hot, cold);
  Run(dispatcher, source, {"SET", hot, "1"});
  Run(dispatcher, source, {"SET", cold, "cold-value"});
  Poll(master, replica, 1);
  Run(dispatcher, source, {"INCR", hot});
  Poll(master, replica, 1);
  test::RequireEqual(replica.ExecuteRead({"GET", cold}, 1'000'000, true).text,
                     "cold-value", "a hot lower slot cannot starve later slots");
}

CACHE_TEST(InterruptedBatchResumesWithoutExposingPartialSlot) {
  cache::CacheEngine source, target;
  command::CommandDispatcher dispatcher;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  Run(dispatcher, source, {"SET", "k", "1"});
  Run(dispatcher, source, {"INCR", "k"});
  auto frames = master.Poll(repl::Frame::Hello("replica", repl::kProtocolVersion,
                                              "", replica.Positions()), 1);
  test::Require(frames.size() == 3, "one log between BEGIN and DONE");
  test::Require(replica.EnqueueFrame(std::move(frames[0])) &&
                    replica.EnqueueFrame(std::move(frames[1])), "partial batch applies");
  test::Require(replica.ExecuteRead({"GET", "k"}, 1'000'000, true).type ==
                    protocol::ResponseType::kError, "no readiness before completion");
  Poll(master, replica);
  test::RequireEqual(replica.ExecuteRead({"GET", "k"}, 1'000'000, true).text,
                     "2", "resume uses applied progress, not last attempted send");
}

CACHE_TEST(SnapshotPreservesObjectsNeededByAnInFlightExpiryExtension) {
  cache::CacheEngine source, target;
  command::CommandDispatcher dispatcher;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  Run(dispatcher, source, {"SET", "ttl", "v", "EX", "1"}, 1'000'000);
  master.SetGlobalBinlogBudget(0);
  master.Maintain();
  Poll(master, replica);
  // This request started before expiry but commits after the snapshot.
  Run(dispatcher, source, {"EXPIRE", "ttl", "100"}, 1'500'000);
  Poll(master, replica);
  test::Require(source.Get("ttl", 3'000'000).has_value(), "primary extended the deadline");
  test::RequireEqual(replica.ExecuteRead({"GET", "ttl"}, 3'000'000, true).text,
                     "v", "snapshot must preserve prerequisites for later committed logs");
}

CACHE_TEST(ReplicationSnapshotBatchHasByteBudgetAndResumes) {
  cache::CacheEngine source, target;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  std::set<std::size_t> slots;
  std::vector<std::string> keys;
  const std::string value(1024 * 1024, 'x');
  for (int i = 0; keys.size() < 20; ++i) {
    const auto key = "batch:" + std::to_string(i);
    if (!slots.insert(common::SlotForKey(key)).second) continue;
    cache::BinlogRecord record;
    record.args = {"SET", key, value};
    source.Set(key, cache::RedisObject::MakeString(value), std::move(record), 1'000'000);
    keys.push_back(key);
  }
  master.SetGlobalBinlogBudget(0);
  master.Maintain();

  auto send = [&](const std::vector<repl::Frame>& frames) {
    for (const auto& frame : frames) {
      test::Require(replica.EnqueueFrame(frame), "replica accepts bounded batch");
    }
  };
  auto hello = repl::Frame::Hello("replica", repl::kProtocolVersion,
                                 replica.CurrentSessionId(), replica.Positions());
  auto first = master.Poll(hello, 4096);
  std::size_t payload_bytes = 0, snapshots = 0;
  for (const auto& frame : first) {
    if (frame.subcmd == repl::Subcmd::kSnapshot) {
      payload_bytes += frame.snapshot_payload.size();
      ++snapshots;
    }
  }
  test::Require(snapshots > 0 && snapshots < keys.size(),
                "one poll must not retain every large snapshot");
  test::Require(payload_bytes <= 16 * 1024 * 1024,
                "batch payload stays within the fixed byte budget");
  send(first);
  hello = repl::Frame::Hello("replica", repl::kProtocolVersion,
                             replica.CurrentSessionId(), replica.Positions());
  send(master.Poll(hello, 4096));
  for (const auto& key : keys) {
    auto read = replica.ExecuteRead({"GET", key}, 1'000'000, true);
    test::Require(read.type == protocol::ResponseType::kBulkString &&
                  read.text == value, "later poll completes every snapshot");
  }
}

CACHE_TEST(ReplicationLogBatchHasByteBudgetAndResumes) {
  cache::CacheEngine source, target;
  repl::MasterReplicator master(&source);
  repl::SlaveReplicator replica(&target);
  const std::string value(1024 * 1024, 'y');
  for (int i = 0; i < 20; ++i) {
    cache::BinlogRecord record;
    record.args = {"SET", "repeated", value};
    source.Set("repeated", cache::RedisObject::MakeString(value),
               std::move(record), 1'000'000);
  }
  auto hello = repl::Frame::Hello("replica", repl::kProtocolVersion,
                                 replica.CurrentSessionId(), replica.Positions());
  auto first = master.Poll(hello, 4096);
  std::size_t logs = 0, bytes = 0;
  for (const auto& frame : first) {
    if (frame.subcmd == repl::Subcmd::kLog) {
      ++logs;
      bytes += cache::EstimateBinlogRecordBytes(frame.record);
    }
    test::Require(replica.EnqueueFrame(frame), "replica applies first log batch");
  }
  test::Require(logs > 0 && logs < 20 && bytes <= 16 * 1024 * 1024,
                "one poll must bound large log records");
  hello = repl::Frame::Hello("replica", repl::kProtocolVersion,
                             replica.CurrentSessionId(), replica.Positions());
  for (const auto& frame : master.Poll(hello, 4096)) {
    test::Require(replica.EnqueueFrame(frame), "replica applies remaining logs");
  }
  auto read = replica.ExecuteRead({"GET", "repeated"}, 1'000'000, true);
  test::Require(read.type == protocol::ResponseType::kBulkString && read.text == value,
                "later poll completes the write sequence");
}
