#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "repl/repl_frame.h"

namespace repl {

class SlaveReplicator {
 public:
  SlaveReplicator(cache::CacheEngine* engine, std::size_t apply_workers);

  void EnqueueFrame(Frame frame);
  void ApplyLogForTest(std::size_t slot_id, const cache::BinlogRecord& record,
                       std::uint64_t now_us);
  std::uint64_t AppliedSeqForTest(std::size_t slot_id) const;

 private:
  struct SlotApplyState {
    std::uint64_t applied_seq = 0;
    std::map<std::uint64_t, cache::BinlogRecord> pending;
  };

  void ApplyLog(std::size_t slot_id, const cache::BinlogRecord& record,
                std::uint64_t now_us);
  bool ApplyRecord(const cache::BinlogRecord& record, std::uint64_t now_us);
  SlotApplyState& StateForSlot(std::size_t slot_id);
  const SlotApplyState* FindStateForSlot(std::size_t slot_id) const;

  cache::CacheEngine* engine_;
  std::size_t apply_workers_;
  std::vector<SlotApplyState> slot_states_;
};

}  // namespace repl
