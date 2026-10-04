#include "repl/slave_replicator.h"

#include <limits>
#include <utility>

#include "common/hash.h"
#include "common/parse_utils.h"
#include "common/time.h"
#include "repl/snapshot_codec.h"

namespace repl {

SlaveReplicator::SlaveReplicator(cache::CacheEngine* engine)
    : engine_(engine), slots_(engine ? engine->SlotCount() : 0) {
  if (engine_) engine_->DisableBinlog();
}

bool SlaveReplicator::EnqueueFrame(Frame frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!engine_) return false;
  if (frame.subcmd == Subcmd::kBegin) {
    if (frame.session_id.empty() ||
        (!frame.reset && frame.session_id != session_id_)) return false;
    if (frame.reset) {
      for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        slots_[slot] = {};
        engine_->InstallSlotReplicaSnapshot(slot, {}, 0);
      }
    }
    session_id_ = frame.session_id;
    return true;
  }
  if (session_id_.empty() || frame.session_id != session_id_) return false;
  if (frame.subcmd == Subcmd::kDone) {
    for (const auto& item : frame.slot_positions) {
      if (item.first >= slots_.size()) return false;
      auto& state = slots_[item.first];
      if (!state.need_snapshot) {
        state.state = state.applied_seq >= item.second ? SlotState::kOnline : SlotState::kCatchingUp;
      }
    }
    return true;
  }
  if (frame.slot_id >= slots_.size()) return false;
  if (frame.subcmd == Subcmd::kSnapshot) return ApplySnapshot(frame);
  if (frame.subcmd == Subcmd::kLog) return ApplyLog(frame.slot_id, frame.record, common::NowMicros());
  return false;
}

bool SlaveReplicator::ApplyLog(std::size_t slot, const cache::BinlogRecord& record,
                               std::uint64_t now) {
  if (slot >= slots_.size()) return false;
  auto& state = slots_[slot];
  if (state.need_snapshot) return false;
  if (record.seq <= state.applied_seq) return true;
  if (record.seq != state.applied_seq + 1 || !ApplyRecord(record, now, slot)) {
    state.need_snapshot = true;
    state.state = SlotState::kOffline;
    return false;
  }
  state.applied_seq = record.seq;
  engine_->MarkSlotReplicaAppliedSeq(slot, record.seq);
  state.state = SlotState::kCatchingUp;
  return true;
}

bool SlaveReplicator::ApplyRecord(const cache::BinlogRecord& record,
                                  std::uint64_t now, std::size_t slot) {
  if (!engine_ || record.args.size() < 2 ||
      common::SlotForKey(record.args[1]) != slot ||
      !dispatcher_.IsWriteCommand(record.args[0])) return false;
  // The primary emits one DEL per key; replay must never write another slot.
  if (common::ToUpperAscii(record.args[0]) == "DEL") {
    if (record.args.size() != 2) return false;
    // DEL also represents primary expiry. Remove the stored object even when
    // its deadline has elapsed; ordinary client DEL only sees live objects.
    engine_->Del(record.args[1], 0);
    return true;
  }
  auto result = dispatcher_.Execute(record.args, *engine_,
                 record.written_at_us ? record.written_at_us : now);
  if (result.type == protocol::ResponseType::kError) return false;
  if (record.written_at_us) {
    engine_->SlotById(slot).SetReplicaDeadline(record.args[1], record.deadline_us);
  }
  return true;
}

bool SlaveReplicator::ApplySnapshot(const Frame& frame) {
  auto& state = slots_[frame.slot_id];
  state.state = SlotState::kSnapshotting;
  auto map = DecodeSnapshotPayload(frame.snapshot_payload);
  bool valid = map.has_value();
  if (map) map->ForEach([&](const cache::PackedString& key, const cache::RedisObject&) {
    if (common::SlotForKey(key.View()) != frame.slot_id) valid = false;
  });
  if (!valid) {
    state.state = SlotState::kOffline;
    state.need_snapshot = true;
    return false;
  }
  engine_->InstallSlotReplicaSnapshot(frame.slot_id, std::move(*map), frame.base_seq);
  state.applied_seq = frame.base_seq;
  state.need_snapshot = false;
  state.state = SlotState::kCatchingUp;
  return true;
}

protocol::Response SlaveReplicator::ExecuteRead(const std::vector<std::string>& args,
                                                std::uint64_t now, bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!engine_) return protocol::Response::Error("TRYAGAIN replica is unavailable");
  if (!args.empty() && dispatcher_.IsWriteCommand(args[0]))
    return protocol::Response::Error("READONLY replica does not accept writes");
  if (!enabled) return protocol::Response::Error("READONLY replica reads are disabled");
  return dispatcher_.ExecuteReadOnly(args, *engine_, now, [&](std::size_t slot) {
    return slot < slots_.size() && slots_[slot].state == SlotState::kOnline;
  });
}

bool SlaveReplicator::CanReadSlot(std::size_t slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return slot < slots_.size() && slots_[slot].state == SlotState::kOnline;
}
std::size_t SlaveReplicator::SlotCount() const { return slots_.size(); }
std::string SlaveReplicator::CurrentSessionId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_id_;
}
std::vector<std::pair<std::size_t, std::uint64_t>> SlaveReplicator::Positions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::size_t, std::uint64_t>> positions;
  positions.reserve(slots_.size());
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    positions.emplace_back(i, slots_[i].need_snapshot ? std::numeric_limits<std::uint64_t>::max()
                                                     : slots_[i].applied_seq);
  }
  return positions;
}

}  // namespace repl
