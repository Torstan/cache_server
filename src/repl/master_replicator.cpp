#include "repl/master_replicator.h"

namespace repl {

MasterReplicator::MasterReplicator(cache::CacheEngine* engine)
    : engine_(engine) {}

void MasterReplicator::OnAck(std::size_t slot_id,
                             std::uint64_t applied_seq) {
  if (engine_ == nullptr || slot_id >= engine_->SlotCount()) {
    return;
  }
  engine_->SlotById(slot_id).AckLogsThrough(applied_seq);
}

std::vector<ResumeSlotPlan> MasterReplicator::BuildResumePlan(
    const std::vector<std::pair<std::size_t, std::uint64_t>>&
        last_applied_seq_by_slot,
    std::size_t log_limit_per_slot) const {
  std::vector<ResumeSlotPlan> plans;
  if (engine_ == nullptr) {
    return plans;
  }

  plans.reserve(last_applied_seq_by_slot.size());
  for (const auto& item : last_applied_seq_by_slot) {
    if (item.first >= engine_->SlotCount()) {
      continue;
    }
    ResumeSlotPlan plan;
    plan.slot_id = item.first;
    plan.last_applied_seq = item.second;
    plan.backlog =
        engine_->SlotById(item.first).CopyLogsAfter(item.second,
                                                    log_limit_per_slot);
    plans.push_back(std::move(plan));
  }
  return plans;
}

}  // namespace repl
