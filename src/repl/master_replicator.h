#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "cache/cache_engine.h"
#include "repl/repl_frame.h"

namespace repl {

struct ReplicaSlotState {
  std::uint64_t acked_seq = 0;
  std::uint64_t sent_seq = 0;
  bool need_snapshot = false;
};

struct ReplicaSession {
  std::string replica_id;
  std::string session_id;
  std::vector<ReplicaSlotState> slots;
  std::size_t next_slot = 0;
  bool reset = false;
};

class MasterReplicator {
 public:
  explicit MasterReplicator(cache::CacheEngine* engine);
  // Owns session mutation and batch construction; callers send after return.
  std::vector<Frame> Poll(const Frame& hello, std::size_t max_frames);
  void Maintain();
  void SetGlobalBinlogBudget(std::size_t bytes);

 private:
  friend struct ReplicatorTestAccess;
  std::string OnHelloUnlocked(const Frame& hello);
  std::vector<Frame> BuildFrames(ReplicaSession& replica, std::size_t max_frames,
                                std::vector<std::pair<std::size_t, std::uint64_t>>* completed);
  ReplicaSession* FindReplica(const std::string& replica_id);
  std::string NewSessionId(const std::string& replica_id);
  bool HasBacklog(std::size_t slot_id, std::uint64_t seq) const;
  Frame BuildSnapshotFrame(const ReplicaSession& replica, std::size_t slot_id) const;
  void CollectGarbage();
  void EnforceBudget();

  cache::CacheEngine* engine_;
  mutable std::mutex mutex_;
  std::string epoch_;
  std::vector<ReplicaSession> replicas_;
  std::uint64_t next_session_seq_ = 1;
  std::size_t global_binlog_budget_bytes_ =
      std::numeric_limits<std::size_t>::max();
};

}  // namespace repl
