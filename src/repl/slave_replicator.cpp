#include "repl/slave_replicator.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "common/hash.h"
#include "common/time.h"
#include "repl/snapshot_codec.h"

namespace repl {

namespace {

std::uint64_t ApplyNowUs() { return common::NowMicros(); }

}  // namespace

SlaveReplicator::SlaveReplicator(cache::CacheEngine* engine,
                                 std::size_t apply_workers)
    : engine_(engine),
      apply_workers_(apply_workers == 0 ? 1 : apply_workers),
      slot_states_(engine == nullptr ? 0 : engine->SlotCount()),
      slot_worker_(engine == nullptr ? 0 : engine->SlotCount(), 0) {
  for (std::size_t slot_id = 0; slot_id < slot_worker_.size(); ++slot_id) {
    slot_worker_[slot_id] = WorkerForSlot(slot_id);
  }
}

void SlaveReplicator::EnqueueFrame(Frame frame) {
  if (frame.subcmd == Subcmd::kSnapshot) {
    if (IsCurrentSession(frame)) {
      ApplySnapshot(frame);
    }
    return;
  }
  if (frame.subcmd != Subcmd::kLog || !IsCurrentSession(frame)) {
    return;
  }
  ApplyLogOnWorker(WorkerForSlot(frame.slot_id), frame.slot_id, frame.record,
                   ApplyNowUs());
}

void SlaveReplicator::ApplyLogForTest(std::size_t slot_id,
                                      const cache::BinlogRecord& record,
                                      std::uint64_t now_us) {
  ApplyLog(slot_id, record, now_us);
}

bool SlaveReplicator::ApplyRecordViaDispatcherForTest(
    const cache::BinlogRecord& record, std::uint64_t now_us) {
  if (record.args.size() < 2) {
    return ApplyRecordViaDispatcher(record, now_us, 0);
  }
  return ApplyRecordViaDispatcher(record, now_us,
                                  common::SlotForKey(record.args[1]));
}

std::uint64_t SlaveReplicator::AppliedSeqForTest(std::size_t slot_id) const {
  const SlotApplyState* state = FindStateForSlot(slot_id);
  return state == nullptr ? 0 : state->applied_seq;
}

std::size_t SlaveReplicator::WorkerForSlotForTest(std::size_t slot_id) const {
  return WorkerForSlot(slot_id);
}

void SlaveReplicator::StartSessionForTest(std::string session_id) {
  session_id_ = std::move(session_id);
}

SlaveReplicator::SlotState SlaveReplicator::SlotStateForTest(
    std::size_t slot_id) const {
  const SlotApplyState* state = FindStateForSlot(slot_id);
  return state == nullptr ? SlotState::kOffline : state->state;
}

bool SlaveReplicator::CanReadSlotForTest(std::size_t slot_id) const {
  return CanReadSlot(slot_id);
}

bool SlaveReplicator::CanReadSlot(std::size_t slot_id) const {
  return SlotStateForTest(slot_id) == SlotState::kOnline;
}

std::size_t SlaveReplicator::SlotCount() const {
  return slot_states_.size();
}

const std::string& SlaveReplicator::CurrentSessionId() const {
  return session_id_;
}

bool SlaveReplicator::IsCurrentSession(const Frame& frame) const {
  return !session_id_.empty() && frame.session_id == session_id_;
}

void SlaveReplicator::ApplyLog(std::size_t slot_id,
                               const cache::BinlogRecord& record,
                               std::uint64_t now_us) {
  ApplyLogOnWorker(WorkerForSlot(slot_id), slot_id, record, now_us);
}

void SlaveReplicator::ApplyLogOnWorker(std::size_t worker_id,
                                       std::size_t slot_id,
                                       const cache::BinlogRecord& record,
                                       std::uint64_t now_us) {
  if (slot_id >= slot_worker_.size()) {
    throw std::out_of_range("slot id out of range");
  }
  slot_worker_[slot_id] = worker_id;

  SlotApplyState& state = StateForSlot(slot_id);
  if (record.seq <= state.applied_seq) {
    return;
  }
  if (record.seq != state.applied_seq + 1) {
    state.pending.emplace(record.seq, record);
    if (state.pending.size() > max_pending_logs_per_slot_) {
      state.pending.clear();
      state.state = SlotState::kOffline;
    } else if (state.state == SlotState::kOnline) {
      state.state = SlotState::kCatchingUp;
    }
    return;
  }

  if (!ApplyRecord(record, now_us, slot_id)) {
    return;
  }
  state.applied_seq = record.seq;
  if (engine_ != nullptr) {
    engine_->MarkSlotReplicaAppliedSeq(slot_id, state.applied_seq);
  }
  state.state = SlotState::kOnline;

  for (;;) {
    auto next = state.pending.find(state.applied_seq + 1);
    if (next == state.pending.end()) {
      return;
    }
    cache::BinlogRecord pending = std::move(next->second);
    state.pending.erase(next);
    if (!ApplyRecord(pending, now_us, slot_id)) {
      state.state = SlotState::kOffline;
      return;
    }
    state.applied_seq = pending.seq;
    if (engine_ != nullptr) {
      engine_->MarkSlotReplicaAppliedSeq(slot_id, state.applied_seq);
    }
    state.state = SlotState::kOnline;
  }
}

bool SlaveReplicator::ApplyRecordViaDispatcher(
    const cache::BinlogRecord& record, std::uint64_t now_us,
    std::size_t slot_id) {
  if (engine_ == nullptr || record.args.size() < 2) {
    return false;
  }

  const std::size_t write_slot = common::SlotForKey(record.args[1]);
  if (write_slot != slot_id) {
    return false;
  }

  std::vector<std::string_view> args;
  args.reserve(record.args.size());
  for (const std::string& arg : record.args) {
    args.push_back(arg);
  }

  command::CommandReplayOptions replay_options;
  replay_options.remaining_ttl_us = record.remaining_ttl_us;
  const bool write_cmd = dispatcher_.IsWriteCommand(record.args[0]);
  command::CommandResult result =
      dispatcher_.ExecuteWithResult(args, *engine_, now_us, replay_options);
  if (result.response.type == protocol::ResponseType::kError) {
    return false;
  }
  if (write_cmd) {
    engine_->SlotById(slot_id).AckLogsThrough(record.seq);
  }
  return write_cmd;
}

bool SlaveReplicator::ApplyRecord(const cache::BinlogRecord& record,
                                  std::uint64_t now_us,
                                  std::size_t slot_id) {
  return ApplyRecordViaDispatcher(record, now_us, slot_id);
}

void SlaveReplicator::ApplySnapshot(const Frame& frame) {
  if (engine_ == nullptr || frame.slot_id >= slot_states_.size()) {
    return;
  }
  SlotApplyState& state = StateForSlot(frame.slot_id);
  state.state = SlotState::kSnapshotting;

  auto decoded = DecodeSnapshotPayload(frame.snapshot_payload);
  if (!decoded.has_value()) {
    state.state = SlotState::kOffline;
    return;
  }

  engine_->InstallSlotReplicaSnapshot(frame.slot_id, std::move(*decoded),
                                      frame.base_seq);
  state.applied_seq = frame.base_seq;
  state.pending.clear();
  state.state = SlotState::kOnline;

  for (;;) {
    auto next = state.pending.find(state.applied_seq + 1);
    if (next == state.pending.end()) {
      return;
    }
    cache::BinlogRecord pending = std::move(next->second);
    state.pending.erase(next);
    if (!ApplyRecord(pending, ApplyNowUs(), frame.slot_id)) {
      state.state = SlotState::kOffline;
      return;
    }
    state.applied_seq = pending.seq;
    engine_->MarkSlotReplicaAppliedSeq(frame.slot_id, state.applied_seq);
  }
}

std::size_t SlaveReplicator::WorkerForSlot(std::size_t slot_id) const {
  return slot_id % apply_workers_;
}

SlaveReplicator::SlotApplyState& SlaveReplicator::StateForSlot(
    std::size_t slot_id) {
  if (slot_id >= slot_states_.size()) {
    throw std::out_of_range("slot id out of range");
  }
  return slot_states_[slot_id];
}

const SlaveReplicator::SlotApplyState* SlaveReplicator::FindStateForSlot(
    std::size_t slot_id) const {
  if (slot_id >= slot_states_.size()) {
    return nullptr;
  }
  return &slot_states_[slot_id];
}

}  // namespace repl
