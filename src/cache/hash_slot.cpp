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

  const bool created = !existing.has_value();
  std::optional<RedisObject> new_obj = updater(std::move(existing));
  if (!new_obj) {
    return WriteResult{Status::kOk, false, false, slot_seq_};
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap next_map = current_map.Set(packed_key, std::move(*new_obj));

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

WriteResult HashSlot::HSet(std::string_view key, std::string_view field,
                           std::string_view value, std::uint64_t now_us) {
  const PackedString packed_key(key);
  const PackedString packed_field(field);
  const PackedString packed_value(value);

  std::lock_guard<std::mutex> write_lock(write_mutex_);
  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap current_map;
  RedisObject object;
  bool found_object = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    current_map = redis_obj_map_;
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found != nullptr) {
      object = *found;
      found_object = true;
    }
  }

  HashValue hash;
  std::uint64_t deadline_us = 0;
  bool created = true;
  if (found_object && !object.IsExpired(now_us)) {
    if (object.Type() != RedisObjectType::kHash || object.Hash() == nullptr) {
      return WriteResult{Status::kWrongType, false, false, slot_seq_};
    }
    hash = *object.Hash();
    deadline_us = object.DeadlineUs();
    created = hash.Find(packed_field) == nullptr;
  }

  HashValue next_hash = hash.Set(packed_field, packed_value);
  RedisObject next_object =
      PreserveDeadline(RedisObject::MakeHash(std::move(next_hash)), deadline_us);
  ObjectMap next_map = current_map.Set(packed_key, next_object);

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kHSet;
  record.args = {"HSET", std::string(key), std::string(field),
                 std::string(value)};

  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }

  return WriteResult{Status::kOk, true, created, next_seq};
}

ReadResult<std::string> HashSlot::HGet(std::string_view key,
                                       std::string_view field,
                                       std::uint64_t now_us) const {
  const PackedString packed_key(key);
  const PackedString packed_field(field);
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
  if (object.Type() != RedisObjectType::kHash || object.Hash() == nullptr) {
    return ReadResult<std::string>{Status::kWrongType, {}};
  }

  const PackedString* found = object.Hash()->Find(packed_field);
  if (found == nullptr) {
    return ReadResult<std::string>{Status::kNotFound, {}};
  }
  return ReadResult<std::string>{Status::kOk, found->ToString()};
}

WriteResult HashSlot::SAdd(std::string_view key, std::string_view member,
                           std::uint64_t now_us) {
  const PackedString packed_key(key);
  const PackedString packed_member(member);

  std::lock_guard<std::mutex> write_lock(write_mutex_);
  ObjectMap current_map;
  RedisObject object;
  bool found_object = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    current_map = redis_obj_map_;
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found != nullptr) {
      object = *found;
      found_object = true;
    }
  }

  SetValue set;
  std::uint64_t deadline_us = 0;
  if (found_object && !object.IsExpired(now_us)) {
    if (object.Type() != RedisObjectType::kSet || object.Set() == nullptr) {
      return WriteResult{Status::kWrongType, false, false, slot_seq_};
    }
    set = *object.Set();
    deadline_us = object.DeadlineUs();
    if (set.Contains(packed_member)) {
      return WriteResult{Status::kOk, false, false, slot_seq_};
    }
  }

  const std::uint64_t next_seq = slot_seq_ + 1;
  SetValue next_set = set.Add(packed_member);
  RedisObject next_object =
      PreserveDeadline(RedisObject::MakeSet(std::move(next_set)), deadline_us);
  ObjectMap next_map = current_map.Set(packed_key, next_object);

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kSAdd;
  record.args = {"SADD", std::string(key), std::string(member)};

  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }

  return WriteResult{Status::kOk, true, true, next_seq};
}

ReadResult<bool> HashSlot::SIsMember(std::string_view key,
                                     std::string_view member,
                                     std::uint64_t now_us) const {
  const PackedString packed_key(key);
  const PackedString packed_member(member);
  RedisObject object;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found == nullptr) {
      return ReadResult<bool>{Status::kNotFound, false};
    }
    object = *found;
  }

  if (object.IsExpired(now_us)) {
    return ReadResult<bool>{Status::kNotFound, false};
  }
  if (object.Type() != RedisObjectType::kSet || object.Set() == nullptr) {
    return ReadResult<bool>{Status::kWrongType, false};
  }
  return ReadResult<bool>{Status::kOk, object.Set()->Contains(packed_member)};
}

WriteResult HashSlot::ZAdd(std::string_view key, double score,
                           std::string_view member, std::uint64_t now_us) {
  const PackedString packed_key(key);
  const PackedString packed_member(member);

  std::lock_guard<std::mutex> write_lock(write_mutex_);
  const std::uint64_t next_seq = slot_seq_ + 1;
  ObjectMap current_map;
  RedisObject object;
  bool found_object = false;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    current_map = redis_obj_map_;
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found != nullptr) {
      object = *found;
      found_object = true;
    }
  }

  ZSetValue zset;
  std::uint64_t deadline_us = 0;
  bool created = true;
  if (found_object && !object.IsExpired(now_us)) {
    if (object.Type() != RedisObjectType::kZSet || object.ZSet() == nullptr) {
      return WriteResult{Status::kWrongType, false, false, slot_seq_};
    }
    zset = *object.ZSet();
    deadline_us = object.DeadlineUs();
    created = zset.Find(packed_member) == nullptr;
  }

  ZSetValue next_zset = zset.Set(packed_member, score);
  RedisObject next_object =
      PreserveDeadline(RedisObject::MakeZSet(std::move(next_zset)), deadline_us);
  ObjectMap next_map = current_map.Set(packed_key, next_object);

  BinlogRecord record;
  record.seq = next_seq;
  record.op = BinlogOp::kZAdd;
  record.args = {"ZADD", std::string(key), FormatScore(score),
                 std::string(member)};

  binlog_buffer_.Append(std::move(record));
  slot_seq_ = next_seq;

  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    redis_obj_map_ = std::move(next_map);
    published_seq_ = next_seq;
  }

  return WriteResult{Status::kOk, true, created, next_seq};
}

ReadResult<double> HashSlot::ZScore(std::string_view key,
                                    std::string_view member,
                                    std::uint64_t now_us) const {
  const PackedString packed_key(key);
  const PackedString packed_member(member);
  RedisObject object;
  {
    std::lock_guard<std::mutex> value_lock(value_mutex_);
    const RedisObject* found = redis_obj_map_.Find(packed_key);
    if (found == nullptr) {
      return ReadResult<double>{Status::kNotFound, 0.0};
    }
    object = *found;
  }

  if (object.IsExpired(now_us)) {
    return ReadResult<double>{Status::kNotFound, 0.0};
  }
  if (object.Type() != RedisObjectType::kZSet || object.ZSet() == nullptr) {
    return ReadResult<double>{Status::kWrongType, 0.0};
  }

  const double* found = object.ZSet()->Find(packed_member);
  if (found == nullptr) {
    return ReadResult<double>{Status::kNotFound, 0.0};
  }
  return ReadResult<double>{Status::kOk, *found};
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
