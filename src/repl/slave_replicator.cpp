#include "repl/slave_replicator.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "cache/redis_object.h"
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

cache::RedisObject PreserveDeadline(cache::RedisObject object,
                                    std::uint64_t deadline_us) {
  if (deadline_us == 0) {
    return object;
  }
  return object.WithDeadline(deadline_us);
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

bool SlaveReplicator::ApplyRecordViaDispatcher(
    const cache::BinlogRecord& record, std::uint64_t now_us) {
  if (engine_ == nullptr || record.args.empty()) {
    return false;
  }

  std::vector<std::string_view> args;
  args.reserve(record.args.size());
  for (const std::string& arg : record.args) {
    args.push_back(arg);
  }

  protocol::Response response = dispatcher_.Execute(args, *engine_, now_us);

  return response.type == protocol::ResponseType::kSimpleString ||
         response.type == protocol::ResponseType::kInteger ||
         response.type == protocol::ResponseType::kBulkString;
}

bool SlaveReplicator::ApplyRecord(const cache::BinlogRecord& record,
                                  std::uint64_t now_us) {
  if (engine_ == nullptr) {
    return false;
  }

  switch (record.op) {
    case cache::BinlogOp::kSet:
      if (record.args.size() == 3) {
        cache::BinlogRecord replay_record = record;
        engine_->Set(record.args[1],
                     cache::RedisObject::MakeString(record.args[2]),
                     std::move(replay_record), now_us);
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
        cache::BinlogRecord replay_record = record;
        engine_->Update(
            record.args[1],
            [&](std::optional<cache::RedisObject> existing)
                -> std::optional<cache::RedisObject> {
              cache::HashValue hash;
              std::uint64_t deadline_us = 0;
              if (existing) {
                const cache::HashValue* existing_hash = existing->Hash();
                if (existing->Type() != cache::RedisObjectType::kHash ||
                    existing_hash == nullptr) {
                  return std::nullopt;
                }
                hash = *existing_hash;
                deadline_us = existing->DeadlineUs();
              }
              cache::HashValue next =
                  hash.Set(cache::PackedString(record.args[2]),
                           cache::PackedString(record.args[3]));
              return PreserveDeadline(
                  cache::RedisObject::MakeHash(std::move(next)), deadline_us);
            },
            std::move(replay_record), now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kSAdd:
      if (record.args.size() == 3) {
        cache::BinlogRecord replay_record = record;
        engine_->Update(
            record.args[1],
            [&](std::optional<cache::RedisObject> existing)
                -> std::optional<cache::RedisObject> {
              cache::SetValue set;
              std::uint64_t deadline_us = 0;
              const cache::PackedString member(record.args[2]);
              if (existing) {
                const cache::SetValue* existing_set = existing->Set();
                if (existing->Type() != cache::RedisObjectType::kSet ||
                    existing_set == nullptr) {
                  return std::nullopt;
                }
                set = *existing_set;
                deadline_us = existing->DeadlineUs();
                if (set.Contains(member)) {
                  return std::nullopt;
                }
              }
              cache::SetValue next = set.Add(member);
              return PreserveDeadline(
                  cache::RedisObject::MakeSet(std::move(next)), deadline_us);
            },
            std::move(replay_record), now_us);
        return true;
      }
      break;
    case cache::BinlogOp::kZAdd:
      if (record.args.size() == 4) {
        double score = 0.0;
        if (ParseDouble(record.args[2], &score)) {
          cache::BinlogRecord replay_record = record;
          engine_->Update(
              record.args[1],
              [&](std::optional<cache::RedisObject> existing)
                  -> std::optional<cache::RedisObject> {
                cache::ZSetValue zset;
                std::uint64_t deadline_us = 0;
                if (existing) {
                  const cache::ZSetValue* existing_zset = existing->ZSet();
                  if (existing->Type() != cache::RedisObjectType::kZSet ||
                      existing_zset == nullptr) {
                    return std::nullopt;
                  }
                  zset = *existing_zset;
                  deadline_us = existing->DeadlineUs();
                }
                cache::ZSetValue next =
                    zset.Set(cache::PackedString(record.args[3]), score);
                return PreserveDeadline(
                    cache::RedisObject::MakeZSet(std::move(next)),
                    deadline_us);
              },
              std::move(replay_record), now_us);
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
