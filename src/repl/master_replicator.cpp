#include "repl/master_replicator.h"

#include <algorithm>
#include <limits>
#include <random>
#include <utility>

#include "repl/snapshot_codec.h"

namespace repl {
namespace {
constexpr std::size_t kMaxBatchPayloadBytes = 16 * 1024 * 1024;
}

MasterReplicator::MasterReplicator(cache::CacheEngine* engine)
    : engine_(engine) {
  std::random_device random;
  epoch_ = std::to_string(random()) + "-" + std::to_string(random());
}

std::string MasterReplicator::NewSessionId(const std::string& replica_id) {
  return epoch_ + "-" + replica_id + "-" + std::to_string(next_session_seq_++);
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

std::string MasterReplicator::OnHelloUnlocked(const Frame& hello) {
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
  replica->reset = replica->session_id.empty() || hello.session_id != replica->session_id;
  if (replica->reset) {
    replica->session_id = NewSessionId(hello.replica_id);
  }
  for (const auto& item : hello.slot_positions) {
    if (item.first >= replica->slots.size()) {
      continue;
    }
    ReplicaSlotState& slot = replica->slots[item.first];
    slot.acked_seq = replica->reset ? 0 : item.second;
    slot.sent_seq = slot.acked_seq;
    const auto published = engine_->SlotById(item.first).Snapshot().published_seq;
    slot.need_snapshot = slot.sent_seq > published || !HasBacklog(item.first, slot.sent_seq + 1);
    if (slot.sent_seq > published) slot.acked_seq = slot.sent_seq = 0;
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
                                           std::size_t slot_id) const {
  cache::SlotSnapshot snapshot = engine_->SlotById(slot_id).Snapshot();
  std::string payload = EncodeSnapshotPayload(snapshot.map);
  return Frame::Snapshot(replica.session_id, slot_id, snapshot.published_seq,
                         std::move(payload));
}


std::vector<Frame> MasterReplicator::BuildFrames(
    ReplicaSession& replica, std::size_t max_frames,
    std::vector<std::pair<std::size_t, std::uint64_t>>* completed) {
  std::vector<Frame> frames;
  const auto count = replica.slots.size();
  const auto start = replica.next_slot;
  std::size_t payload_bytes = 0;
  bool full = false;
  auto fits = [&](std::size_t bytes) {
    return frames.empty() ||
           (payload_bytes < kMaxBatchPayloadBytes &&
            bytes <= kMaxBatchPayloadBytes - payload_bytes);
  };
  for (std::size_t i = 0; i < count; ++i) {
    const auto slot_id = (start + i) % count;
    auto& state = replica.slots[slot_id];
    auto& slot = engine_->SlotById(slot_id);
    auto target = slot.Snapshot().published_seq;
    if (!full && frames.size() < max_frames) {
      if (state.need_snapshot || !HasBacklog(slot_id, state.sent_seq + 1)) {
        auto frame = BuildSnapshotFrame(replica, slot_id);
        const auto bytes = frame.snapshot_payload.size();
        if (!fits(bytes)) {
          replica.next_slot = slot_id;
          full = true;
        } else {
          payload_bytes += bytes;
          // Use the version of the actual image, never a second live snapshot.
          target = state.sent_seq = frame.base_seq;
          state.need_snapshot = false;
          frames.push_back(std::move(frame));
        }
      } else {
        auto logs = slot.CopyLogsAfter(state.sent_seq,
                                      std::min<std::size_t>(128, max_frames - frames.size()),
                                      kMaxBatchPayloadBytes - payload_bytes);
        for (auto& log : logs) {
          if (log.seq > target) break;
          if (log.seq != state.sent_seq + 1) {
            state.need_snapshot = true;
            break;
          }
          const auto bytes = cache::EstimateBinlogRecordBytes(log);
          if (!fits(bytes)) {
            replica.next_slot = slot_id;
            full = true;
            break;
          }
          payload_bytes += bytes;
          state.sent_seq = log.seq;
          frames.push_back(Frame::Log(replica.session_id, slot_id, std::move(log)));
        }
      }
    }
    if (!full && (frames.size() >= max_frames ||
                  payload_bytes >= kMaxBatchPayloadBytes)) {
      replica.next_slot = (slot_id + 1) % count;
      full = true;
    }
    if (completed && !state.need_snapshot) completed->emplace_back(slot_id, target);
  }
  return frames;
}

std::vector<Frame> MasterReplicator::Poll(const Frame& hello,
                                          std::size_t max_frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!engine_ || hello.subcmd != Subcmd::kHello ||
      hello.proto_version != kProtocolVersion || hello.replica_id.empty()) return {};
  OnHelloUnlocked(hello);
  auto& replica = *FindReplica(hello.replica_id);
  std::vector<std::pair<std::size_t, std::uint64_t>> completed;
  auto frames = BuildFrames(replica, max_frames, &completed);
  frames.insert(frames.begin(), Frame::Begin(replica.session_id, replica.reset));
  frames.push_back(Frame::Done(replica.session_id, std::move(completed)));
  return frames;
}

void MasterReplicator::Maintain() {
  std::lock_guard<std::mutex> lock(mutex_);
  CollectGarbage();
  EnforceBudget();
}

void MasterReplicator::SetGlobalBinlogBudget(std::size_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  global_binlog_budget_bytes_ = bytes;
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
  for (std::size_t slot_id = 0;
       slot_id < engine_->SlotCount() && total > global_binlog_budget_bytes_;
       ++slot_id) {
    auto& slot = engine_->SlotById(slot_id);
    const auto through = slot.MaxRetainedLogSeq();
    const auto removed = slot.AckLogsThroughAndCountBytes(through);
    total -= std::min(total, removed);
    for (auto& replica : replicas_) {
      if (replica.slots[slot_id].acked_seq < through) {
        replica.slots[slot_id].need_snapshot = true;
        replica.slots[slot_id].sent_seq = replica.slots[slot_id].acked_seq;
      }
    }
  }
}


}  // namespace repl
