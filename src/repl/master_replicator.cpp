#include "repl/master_replicator.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "repl/snapshot_codec.h"

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

std::string MasterReplicator::NewSessionId(const std::string& replica_id) {
  return replica_id + "-" + std::to_string(next_session_seq_++);
}

ReplicaSession* MasterReplicator::FindReplica(
    const std::string& replica_id) {
  for (ReplicaSession& replica : replicas_) {
    if (replica.replica_id == replica_id) {
      return &replica;
    }
  }
  return nullptr;
}

const ReplicaSession* MasterReplicator::FindReplica(
    const std::string& replica_id) const {
  for (const ReplicaSession& replica : replicas_) {
    if (replica.replica_id == replica_id) {
      return &replica;
    }
  }
  return nullptr;
}

std::string MasterReplicator::OnHello(const Frame& hello) {
  if (engine_ == nullptr || hello.subcmd != Subcmd::kHello) {
    return "";
  }
  ReplicaSession* replica = FindReplica(hello.replica_id);
  if (replica == nullptr) {
    ReplicaSession created;
    created.replica_id = hello.replica_id;
    created.slots.resize(engine_->SlotCount());
    replicas_.push_back(std::move(created));
    replica = &replicas_.back();
  }
  replica->session_id = NewSessionId(hello.replica_id);
  for (const auto& item : hello.slot_positions) {
    if (item.first >= replica->slots.size()) {
      continue;
    }
    ReplicaSlotState& slot = replica->slots[item.first];
    slot.acked_seq = item.second;
    slot.sent_seq = item.second;
    slot.need_snapshot = !HasBacklog(item.first, item.second + 1);
  }
  return replica->session_id;
}

bool MasterReplicator::HasBacklog(std::size_t slot_id,
                                  std::uint64_t seq) const {
  if (engine_ == nullptr || slot_id >= engine_->SlotCount()) {
    return false;
  }
  const cache::HashSlot& slot = engine_->SlotById(slot_id);
  const std::uint64_t min_seq = slot.MinRetainedLogSeq();
  const std::uint64_t max_seq = slot.MaxRetainedLogSeq();
  if (seq == 0) {
    return true;
  }
  if (min_seq == 0 && max_seq == 0) {
    // Buffer is empty: only "caught up" replicas need no backlog or snapshot.
    return seq > slot.Snapshot().published_seq;
  }
  return seq >= min_seq && seq <= max_seq + 1;
}

Frame MasterReplicator::BuildSnapshotFrame(const ReplicaSession& replica,
                                           std::size_t slot_id,
                                           std::uint64_t now_us) const {
  cache::SlotSnapshot snapshot = engine_->SlotById(slot_id).Snapshot();
  std::string payload = EncodeSnapshotPayload(snapshot.map, now_us);
  return Frame::Snapshot(replica.session_id, slot_id, snapshot.published_seq,
                         std::move(payload));
}

std::vector<Frame> MasterReplicator::BuildFramesForReplica(
    const std::string& replica_id, std::size_t max_frames,
    std::uint64_t now_us) {
  std::vector<Frame> frames;
  if (engine_ == nullptr || max_frames == 0) {
    return frames;
  }
  ReplicaSession* replica = FindReplica(replica_id);
  if (replica == nullptr) {
    return frames;
  }
  for (std::size_t slot_id = 0;
       slot_id < replica->slots.size() && frames.size() < max_frames;
       ++slot_id) {
    ReplicaSlotState& slot = replica->slots[slot_id];
    if (slot.need_snapshot) {
      frames.push_back(BuildSnapshotFrame(*replica, slot_id, now_us));
      slot.sent_seq = engine_->SlotById(slot_id).Snapshot().published_seq;
      slot.need_snapshot = false;
      continue;
    }
    const std::uint64_t after = slot.sent_seq;
    std::vector<cache::BinlogRecord> logs =
        engine_->SlotById(slot_id).CopyLogsAfter(after, 1);
    if (logs.empty()) {
      continue;
    }
    slot.sent_seq = logs[0].seq;
    frames.push_back(Frame::Log(replica->session_id, slot_id,
                                std::move(logs[0])));
  }
  return frames;
}

void MasterReplicator::OnAck(const Frame& ack) {
  if (ack.subcmd != Subcmd::kAck) {
    return;
  }
  for (ReplicaSession& replica : replicas_) {
    if (replica.session_id != ack.session_id) {
      continue;
    }
    for (const auto& item : ack.acked_slots) {
      if (item.first < replica.slots.size() &&
          item.second > replica.slots[item.first].acked_seq) {
        replica.slots[item.first].acked_seq = item.second;
      }
    }
    return;
  }
}

void MasterReplicator::CollectGarbage() {
  if (engine_ == nullptr || replicas_.empty()) {
    return;
  }
  for (std::size_t slot_id = 0; slot_id < engine_->SlotCount(); ++slot_id) {
    std::uint64_t min_ack = std::numeric_limits<std::uint64_t>::max();
    for (const ReplicaSession& replica : replicas_) {
      min_ack = std::min(min_ack, replica.slots[slot_id].acked_seq);
    }
    if (min_ack != std::numeric_limits<std::uint64_t>::max()) {
      engine_->SlotById(slot_id).AckLogsThrough(min_ack);
    }
  }
}

void MasterReplicator::CollectGarbageForTest() { CollectGarbage(); }

void MasterReplicator::SetGlobalBinlogBudgetForTest(std::size_t bytes) {
  global_binlog_budget_bytes_ = bytes;
}

void MasterReplicator::EnforceBudget() {
  if (engine_ == nullptr) {
    return;
  }
  std::size_t total = 0;
  for (std::size_t slot_id = 0; slot_id < engine_->SlotCount(); ++slot_id) {
    total += engine_->SlotById(slot_id).RetainedLogBytes();
  }
  if (total <= global_binlog_budget_bytes_) {
    return;
  }
  for (ReplicaSession& replica : replicas_) {
    for (std::size_t slot_id = 0; slot_id < replica.slots.size(); ++slot_id) {
      if (engine_->SlotById(slot_id).RetainedLogBytes() == 0) {
        continue;
      }
      replica.slots[slot_id].need_snapshot = true;
      replica.slots[slot_id].sent_seq = replica.slots[slot_id].acked_seq;
      return;
    }
  }
}

void MasterReplicator::EnforceBudgetForTest() { EnforceBudget(); }

}  // namespace repl
