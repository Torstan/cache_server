#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "repl/repl_frame.h"

namespace repl {

class SlaveReplicator {
 public:
  enum class SlotState { kOffline, kSnapshotting, kCatchingUp, kOnline };

  SlaveReplicator(cache::CacheEngine* engine, std::size_t apply_workers);

  void EnqueueFrame(Frame frame);
  void ApplyLogForTest(std::size_t slot_id, const cache::BinlogRecord& record,
                       std::uint64_t now_us);
  bool ApplyRecordViaDispatcherForTest(const cache::BinlogRecord& record,
                                       std::uint64_t now_us);
  std::uint64_t AppliedSeqForTest(std::size_t slot_id) const;
  std::size_t WorkerForSlotForTest(std::size_t slot_id) const;

  void StartSessionForTest(std::string session_id);
  SlotState SlotStateForTest(std::size_t slot_id) const;
  bool CanReadSlot(std::size_t slot_id) const;
  bool CanReadSlotForTest(std::size_t slot_id) const;
  std::size_t SlotCount() const;
  const std::string& CurrentSessionId() const;

 private:
  struct SlotApplyState {
    SlotState state = SlotState::kOffline;
    std::uint64_t applied_seq = 0;
    std::map<std::uint64_t, cache::BinlogRecord> pending;
  };

  void ApplyLog(std::size_t slot_id, const cache::BinlogRecord& record,
                std::uint64_t now_us);
  void ApplyLogOnWorker(std::size_t worker_id, std::size_t slot_id,
                        const cache::BinlogRecord& record,
                        std::uint64_t now_us);
  bool ApplyRecordViaDispatcher(const cache::BinlogRecord& record,
                                std::uint64_t now_us, std::size_t slot_id);
  bool ApplyRecord(const cache::BinlogRecord& record, std::uint64_t now_us,
                   std::size_t slot_id);
  void ApplySnapshot(const Frame& frame);
  bool IsCurrentSession(const Frame& frame) const;
  std::size_t WorkerForSlot(std::size_t slot_id) const;
  SlotApplyState& StateForSlot(std::size_t slot_id);
  const SlotApplyState* FindStateForSlot(std::size_t slot_id) const;

  cache::CacheEngine* engine_;
  command::CommandDispatcher dispatcher_;
  std::size_t apply_workers_;
  std::vector<SlotApplyState> slot_states_;
  std::vector<std::size_t> slot_worker_;
  std::string session_id_;
  std::size_t max_pending_logs_per_slot_ = 1024;
};

}  // namespace repl
