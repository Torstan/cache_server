#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "repl/repl_frame.h"

namespace repl {

// One polling thread applies frames. Client reads hold the same mutex for the
// entire command, so readiness, data and sequence cannot change mid-read.
class SlaveReplicator {
 public:
  enum class SlotState { kOffline, kSnapshotting, kCatchingUp, kOnline };
  explicit SlaveReplicator(cache::CacheEngine* engine);
  bool EnqueueFrame(Frame frame);
  protocol::Response ExecuteRead(const std::vector<std::string>& args,
                                 std::uint64_t now_us, bool reads_enabled);
  bool CanReadSlot(std::size_t slot_id) const;
  std::size_t SlotCount() const;
  std::string CurrentSessionId() const;
  std::vector<std::pair<std::size_t, std::uint64_t>> Positions() const;

 private:
  friend struct ReplicatorTestAccess;
  struct SlotApplyState {
    SlotState state = SlotState::kOffline;
    std::uint64_t applied_seq = 0;
    bool need_snapshot = false;
  };
  bool ApplyLog(std::size_t slot, const cache::BinlogRecord& record, std::uint64_t now);
  bool ApplyRecord(const cache::BinlogRecord& record, std::uint64_t now, std::size_t slot);
  bool ApplySnapshot(const Frame& frame);

  cache::CacheEngine* engine_;
  command::CommandDispatcher dispatcher_;
  mutable std::mutex mutex_;
  std::vector<SlotApplyState> slots_;
  std::string session_id_;
};
}
