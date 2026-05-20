#include "repl/slave_replicator.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "common/time.h"

namespace repl {

namespace {

constexpr std::uint64_t kMicrosPerSecond = 1000000ULL;

std::uint64_t ApplyNowUs() { return common::NowMicros(); }

bool ParseInt64(const std::string& text, std::int64_t* out) {
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, *out);
  return result.ec == std::errc() && result.ptr == end;
}

bool ParseDouble(const std::string& text, double* out) {
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, *out);
  return result.ec == std::errc() && result.ptr == end && std::isfinite(*out);
}

bool RelativeExpireSeconds(const cache::BinlogRecord& record,
                           std::int64_t* seconds) {
  if (record.remaining_ttl_us > 0) {
    const std::uint64_t rounded =
        record.remaining_ttl_us / kMicrosPerSecond +
        (record.remaining_ttl_us % kMicrosPerSecond == 0 ? 0 : 1);
    if (rounded >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      *seconds = std::numeric_limits<std::int64_t>::max();
    } else {
      *seconds = static_cast<std::int64_t>(rounded);
    }
    return true;
  }
  if (record.args.size() == 3) {
    return ParseInt64(record.args[2], seconds);
  }
  return false;
}

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
  if (frame.subcmd != Subcmd::kLog) {
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

std::uint64_t SlaveReplicator::AppliedSeqForTest(std::size_t slot_id) const {
  const SlotApplyState* state = FindStateForSlot(slot_id);
  return state == nullptr ? 0 : state->applied_seq;
}

std::size_t SlaveReplicator::WorkerForSlotForTest(std::size_t slot_id) const {
  return WorkerForSlot(slot_id);
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
    return;
  }

  if (!ApplyRecord(record, now_us)) {
    return;
  }
  state.applied_seq = record.seq;

  for (;;) {
    auto next = state.pending.find(state.applied_seq + 1);
    if (next == state.pending.end()) {
      return;
    }
    cache::BinlogRecord pending = std::move(next->second);
    state.pending.erase(next);
    if (!ApplyRecord(pending, now_us)) {
      return;
    }
    state.applied_seq = pending.seq;
  }
}

bool SlaveReplicator::ApplyRecord(const cache::BinlogRecord& record,
                                  std::uint64_t now_us) {
  if (engine_ == nullptr) {
    return false;
  }

  switch (record.op) {
    case cache::BinlogOp::kSet:
      if (record.args.size() == 3) {
        engine_->SetString(record.args[1], record.args[2], now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kDel:
      if (record.args.size() == 2) {
        engine_->Del(record.args[1], now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kExpire:
      if (record.args.size() == 2 || record.args.size() == 3) {
        std::int64_t seconds = 0;
        if (!RelativeExpireSeconds(record, &seconds)) {
          return false;
        }
        engine_->Expire(record.args[1], seconds, now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kHSet:
      if (record.args.size() == 4) {
        engine_->HSet(record.args[1], record.args[2], record.args[3], now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kSAdd:
      if (record.args.size() == 3) {
        engine_->SAdd(record.args[1], record.args[2], now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kZAdd:
      if (record.args.size() == 4) {
        double score = 0.0;
        if (ParseDouble(record.args[2], &score)) {
          engine_->ZAdd(record.args[1], score, record.args[3], now_us);
          return true;
        }
      }
      break;
  }

  return false;
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
