#pragma once

#include "repl/master_replicator.h"
#include "repl/slave_replicator.h"

namespace repl {
// Unit tests exercise individual state transitions; network regression tests
// use Poll/EnqueueFrame and the actual wire format instead of this accessor.
struct ReplicatorTestAccess {
  static void ApplyLog(SlaveReplicator& slave, std::size_t slot,
                       const cache::BinlogRecord& record, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(slave.mutex_);
    slave.ApplyLog(slot, record, now);
  }
  static bool ApplyRecord(SlaveReplicator& slave, const cache::BinlogRecord& record,
                           std::uint64_t now) {
    std::lock_guard<std::mutex> lock(slave.mutex_);
    return record.args.size() >= 2 && slave.ApplyRecord(record, now, common::SlotForKey(record.args[1]));
  }
  static std::uint64_t AppliedSeq(const SlaveReplicator& slave, std::size_t slot) {
    std::lock_guard<std::mutex> lock(slave.mutex_);
    return slot < slave.slots_.size() ? slave.slots_[slot].applied_seq : 0;
  }
  static void StartSession(SlaveReplicator& slave, std::string session) {
    std::lock_guard<std::mutex> lock(slave.mutex_);
    slave.session_id_ = std::move(session);
  }
  static SlaveReplicator::SlotState State(const SlaveReplicator& slave, std::size_t slot) {
    std::lock_guard<std::mutex> lock(slave.mutex_);
    return slot < slave.slots_.size() ? slave.slots_[slot].state : SlaveReplicator::SlotState::kOffline;
  }
  static std::string Hello(MasterReplicator& master, const Frame& hello) {
    std::lock_guard<std::mutex> lock(master.mutex_);
    return master.OnHelloUnlocked(hello);
  }
  static std::vector<Frame> Frames(MasterReplicator& master, const std::string& id,
                                    std::size_t limit) {
    std::lock_guard<std::mutex> lock(master.mutex_);
    auto* replica = master.FindReplica(id);
    return replica ? master.BuildFrames(*replica, limit, nullptr) : std::vector<Frame>{};
  }
};
}
