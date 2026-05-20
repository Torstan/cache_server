#include "test_harness.h"

#include "cache/cache_engine.h"
#include "redis/resp.h"
#include "repl/master_replicator.h"
#include "repl/repl_frame.h"

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
  engine.SetString("k", "v1", 100);
  engine.SetString("k", "v2", 200);

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
