#include "cache/hash_slot.h"

#include <string>
#include <utility>

namespace cache {

WriteResult HashSlot::SetString(std::string_view key, std::string_view value,
                                std::uint64_t now_us) {
  (void)now_us;
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  const PackedString packed_key(key);
  const RedisObject object = RedisObject::MakeString(value);
  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map;
  bool created = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    created = redis_obj_map_.Find(packed_key) == nullptr;
    next_map = redis_obj_map_.Set(packed_key, object);
  }

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kSet;
  record.args = {"SET", std::string(key), std::string(value)};

  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }

  return WriteResult{
      Status::kOk,
      true,
      created,
      next_seq,
  };
}

ReadResult<std::string> HashSlot::GetString(std::string_view key,
                                            std::uint64_t now_us) const {
  const PackedString packed_key(key);
  RedisObject object;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found == nullptr) {
      return ReadResult<std::string>{Status::kNotFound, {}};
    }
    object = *found;
  }

  if (object.IsExpired(now_us)) {
    return ReadResult<std::string>{Status::kNotFound, {}};
  }

  if (object.Type() != RedisObjectType::kString) {
    return ReadResult<std::string>{Status::kWrongType, {}};
  }

  const PackedString* value = object.StringValue();
  if (value == nullptr) {
    return ReadResult<std::string>{Status::kWrongType, {}};
  }
  return ReadResult<std::string>{Status::kOk, value->ToString()};
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

bool HashSlot::Expire(std::string_view key, std::int64_t seconds,
                      std::uint64_t now_us) {
  if (seconds <= 0) {
    return Del(key, now_us).changed;
  }

  const PackedString packed_key(key);
  const std::uint64_t deadline_us =
      now_us + static_cast<std::uint64_t>(seconds) * 1'000'000ULL;

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

std::vector<BinlogRecord> HashSlot::CopyLogsAfter(std::uint64_t seq,
                                                  std::size_t limit) const {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  return binlog_buffer_.CopyAfter(seq, limit);
}

void HashSlot::AckLogsThrough(std::uint64_t seq) {
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  binlog_buffer_.AckThrough(seq);
}

}  // namespace cache
