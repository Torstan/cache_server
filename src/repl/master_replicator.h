#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "cache/cache_engine.h"
#include "repl/repl_frame.h"

namespace repl {

struct ResumeSlotPlan {
  std::size_t slot_id = 0;
  std::uint64_t last_applied_seq = 0;
  std::vector<cache::BinlogRecord> backlog;
};

struct ReplicaSlotState {
  std::uint64_t acked_seq = 0;
  std::uint64_t sent_seq = 0;
  bool need_snapshot = false;
};

struct ReplicaSession {
  std::string replica_id;
  std::string session_id;
  std::vector<ReplicaSlotState> slots;
};

class MasterReplicator {
 public:
  explicit MasterReplicator(cache::CacheEngine* engine);

  void OnAck(std::size_t slot_id, std::uint64_t applied_seq);
  std::vector<ResumeSlotPlan> BuildResumePlan(
      const std::vector<std::pair<std::size_t, std::uint64_t>>&
          last_applied_seq_by_slot,
      std::size_t log_limit_per_slot = 1024) const;

  std::string OnHello(const Frame& hello);
  void OnAck(const Frame& ack);
  std::vector<Frame> BuildFramesForReplica(const std::string& replica_id,
                                           std::size_t max_frames,
                                           std::uint64_t now_us);
  void CollectGarbageForTest();
  void EnforceBudgetForTest();
  void SetGlobalBinlogBudgetForTest(std::size_t bytes);

 private:
  ReplicaSession* FindReplica(const std::string& replica_id);
  const ReplicaSession* FindReplica(const std::string& replica_id) const;
  std::string NewSessionId(const std::string& replica_id);
  bool HasBacklog(std::size_t slot_id, std::uint64_t seq) const;
  Frame BuildSnapshotFrame(const ReplicaSession& replica, std::size_t slot_id,
                           std::uint64_t now_us) const;
  void CollectGarbage();
  void EnforceBudget();

  cache::CacheEngine* engine_;
  std::vector<ReplicaSession> replicas_;
  std::uint64_t next_session_seq_ = 1;
  std::size_t global_binlog_budget_bytes_ =
      std::numeric_limits<std::size_t>::max();
};

}  // namespace repl
