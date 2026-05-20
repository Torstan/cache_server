#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cache/cache_engine.h"
#include "repl/repl_frame.h"

namespace repl {

struct ResumeSlotPlan {
  std::size_t slot_id = 0;
  std::uint64_t last_applied_seq = 0;
  std::vector<cache::BinlogRecord> backlog;
};

class MasterReplicator {
 public:
  explicit MasterReplicator(cache::CacheEngine* engine);

  void OnAck(std::size_t slot_id, std::uint64_t applied_seq);
  std::vector<ResumeSlotPlan> BuildResumePlan(
      const std::vector<std::pair<std::size_t, std::uint64_t>>&
          last_applied_seq_by_slot,
      std::size_t log_limit_per_slot = 1024) const;

 private:
  cache::CacheEngine* engine_;
};

}  // namespace repl
