#include "cache/hash_slot.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cache {
namespace {

constexpr std::uint64_t kMicrosPerSecond = 1'000'000ULL;

std::uint64_t SaturatingTtlUs(std::int64_t seconds) {
  const std::uint64_t seconds_u = static_cast<std::uint64_t>(seconds);
  if (seconds_u >
      std::numeric_limits<std::uint64_t>::max() / kMicrosPerSecond) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return seconds_u * kMicrosPerSecond;
}

std::uint64_t SaturatingDeadlineUs(std::uint64_t now_us,
                                   std::uint64_t ttl_us) {
  if (ttl_us > std::numeric_limits<std::uint64_t>::max() - now_us) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return now_us + ttl_us;
}

std::string FormatScore(double score) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << score;
  std::string text = out.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

RedisObject PreserveDeadline(RedisObject object, std::uint64_t deadline_us) {
  if (deadline_us == 0) {
    return object;
  }
  return object.WithDeadline(deadline_us);
}

}  // namespace

std::optional<RedisObject> HashSlot::Get(std::string_view key,
                                         std::uint64_t now_us) const {
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> value_lock(value_mutex_);
  const RedisObject* found = redis_obj_map_.Find(packed_key);
  if (found == nullptr || found->IsExpired(now_us)) {
    return std::nullopt;
  }
  return *found;
}

void HashSlot::ForEachLiveObject(
    std::uint64_t now_us,
    const std::function<void(const PackedString&, const RedisObject&)>& visitor)
    const {
  ObjectMap snapshot;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    snapshot = redis_obj_map_;
  }

  snapshot.ForEach([&](const PackedString& key, const RedisObject& object) {
    if (!object.IsExpired(now_us)) {
      visitor(key, object);
    }
  });
}

WriteResult HashSlot::Set(std::string_view key, RedisObject obj,
                          BinlogRecord record, std::uint64_t now_us) {
  (void)now_us;
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  ObjectMap next_map;
  bool created = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    created = redis_obj_map_.Find(packed_key) == nullptr;
    next_map = redis_obj_map_.Set(packed_key, std::move(obj));
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  record.seq = next_seq;
  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }
  return WriteResult{Status::kOk, true, created, next_seq};
}

WriteResult HashSlot::Update(
    std::string_view key,
    std::function<std::optional<RedisObject>(std::optional<RedisObject>)>
        updater,
    BinlogRecord record, std::uint64_t now_us) {
  return Mutate(
      key,
      [&](std::optional<RedisObject> existing) {
        std::optional<RedisObject> next = updater(std::move(existing));
        return MutationResult{next.has_value(), std::move(next)};
      },
      std::move(record), now_us);
}

WriteResult HashSlot::Mutate(
    std::string_view key,
    std::function<MutationResult(std::optional<RedisObject>)> mutator,
    BinlogRecord record, std::uint64_t now_us) {
  const PackedString packed_key(key);
  std::lock_guard<std::mutex> write_lock(write_mutex_);

  ObjectMap current_map;
  std::optional<RedisObject> existing;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    current_map = redis_obj_map_;
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found != nullptr && !found->IsExpired(now_us)) {
      existing = *found;
    }
  }

  const bool had_existing = existing.has_value();
  MutationResult mutation = mutator(std::move(existing));
  if (!mutation.changed) {
    return WriteResult{Status::kOk, false, false, slot_seq_};
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map;
  if (mutation.object.has_value()) {
    next_map = current_map.Set(packed_key, std::move(*mutation.object));
  } else {
    if (!had_existing) {
      return WriteResult{Status::kOk, false, false, slot_seq_};
    }
    auto erased = current_map.Erase(packed_key);
    if (!erased.has_value()) {
      return WriteResult{Status::kOk, false, false, slot_seq_};
    }
    next_map = std::move(*erased);
  }

  record.seq = next_seq;
  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }
  return WriteResult{Status::kOk, true,
                     !had_existing && mutation.object.has_value(), next_seq};
}

WriteResult HashSlot::Del(std::string_view key, std::uint64_t now_us) {
  const PackedString packed_key(key);

  std::lock_guard<std::mutex> write_lock(write_mutex_);
  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found == nullptr || found->IsExpired(now_us)) {
      return WriteResult{Status::kOk, false, false, slot_seq_};
    }
    auto erased = redis_obj_map_.Erase(packed_key);
    if (!erased.has_value()) {
      return WriteResult{Status::kOk, false, false, slot_seq_};
    }
    next_map = *erased;
  }

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kDel;
  record.args = {"DEL", std::string(key)};

  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }

  return WriteResult{Status::kOk, true, false, next_seq};
}

std::size_t HashSlot::DeleteExpired(std::size_t max_keys,
                                    std::uint64_t now_us) {
  if (max_keys == 0) {
    return 0;
  }

  ObjectMap snapshot;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    snapshot = redis_obj_map_;
  }

  std::vector<std::string> expired_keys;
  expired_keys.reserve(max_keys);
  snapshot.ForEach([&](const PackedString& key, const RedisObject& object) {
    if (expired_keys.size() >= max_keys) {
      return;
    }
    if (object.IsExpired(now_us)) {
      expired_keys.push_back(key.ToString());
    }
  });

  std::size_t deleted = 0;
  for (const std::string& key : expired_keys) {
    const PackedString packed_key(key);
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    const std::uint64_t next_seq = slot_seq_ + 1;
    ObjectMap next_map;

    {
      std::lock_guard<std::mutex> value_lock(value_mutex_);
      const RedisObject* found = redis_obj_map_.Find(packed_key);
      if (found == nullptr || !found->IsExpired(now_us)) {
        continue;
      }
      auto erased = redis_obj_map_.Erase(packed_key);
      if (!erased.has_value()) {
        continue;
      }
      next_map = *erased;
    }

    BinlogRecord record;
    record.seq = next_seq;
    record.op = BinlogOp::kDel;
    record.args = {"DEL", key};

    binlog_buffer_.Append(std::move(record));
    slot_seq_ = next_seq;

    {
      std::lock_guard<std::mutex> value_lock(value_mutex_);
      redis_obj_map_ = std::move(next_map);
      published_seq_ = next_seq;
    }
    ++deleted;
  }
  return deleted;
}

bool HashSlot::Expire(std::string_view key, std::int64_t seconds,
                      std::uint64_t now_us) {
  if (seconds <= 0) {
    return Del(key, now_us).changed;
  }

  const PackedString packed_key(key);
  std::uint64_t ttl_us = SaturatingTtlUs(seconds);
  const std::uint64_t deadline_us = SaturatingDeadlineUs(now_us, ttl_us);
  if (deadline_us == std::numeric_limits<std::uint64_t>::max()) {
    ttl_us = deadline_us - now_us;
  }

  std::lock_guard<std::mutex> write_lock(write_mutex_);
  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found == nullptr || found->IsExpired(now_us)) {
      return false;
    }
    next_map = redis_obj_map_.Set(packed_key, found->WithDeadline(deadline_us));
  }

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kExpire;
  record.args = {"EXPIRE", std::string(key), std::to_string(seconds)};
  record.remaining_ttl_us = ttl_us;

  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }

  return true;
}

std::int64_t HashSlot::Ttl(std::string_view key, std::uint64_t now_us) const {
  const PackedString packed_key(key);
  RedisObject object;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found == nullptr) {
      return -2;
    }
    object = *found;
  }

  if (object.IsExpired(now_us)) {
    return -2;
  }
  if (object.DeadlineUs() == 0) {
    return -1;
  }
  return static_cast<std::int64_t>((object.DeadlineUs() - now_us) /
                                  1'000'000ULL);
}

SlotSnapshot HashSlot::Snapshot() const {
  std::lock_guard<std::mutex> value_lock(value_mutex_);
  return SlotSnapshot{redis_obj_map_, published_seq_};
}

void HashSlot::InstallReplicaSnapshot(ObjectMap map, std::uint64_t seq) {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  binlog_buffer_.Clear();
  slot_seq_ = seq;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(map);
    published_seq_ = seq;
  }
}

void HashSlot::MarkReplicaAppliedSeq(std::uint64_t seq) {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  if (seq <= slot_seq_) {
    return;
  }
  slot_seq_ = seq;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    if (seq > published_seq_) {
      published_seq_ = seq;
    }
  }
}

std::vector<BinlogRecord> HashSlot::CopyLogsAfter(std::uint64_t seq,
                                                  std::size_t limit) const {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  return binlog_buffer_.CopyAfter(seq, limit);
}

std::uint64_t HashSlot::MinRetainedLogSeq() const {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  return binlog_buffer_.MinSeq();
}

std::uint64_t HashSlot::MaxRetainedLogSeq() const {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  return binlog_buffer_.MaxSeq();
}

std::size_t HashSlot::RetainedLogBytes() const {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  return binlog_buffer_.RetainedBytes();
}

std::size_t HashSlot::AckLogsThroughAndCountBytes(std::uint64_t seq) {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  return binlog_buffer_.AckThroughAndCountBytes(seq);
}

void HashSlot::AckLogsThrough(std::uint64_t seq) {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  binlog_buffer_.AckThrough(seq);
}

}  // namespace cache
