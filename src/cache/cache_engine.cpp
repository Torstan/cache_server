#include "cache/cache_engine.h"

#include <utility>

namespace cache {

CacheEngine::CacheEngine() {
  slots_.reserve(common::kSlotCount);
  for (std::size_t i = 0; i < common::kSlotCount; ++i) {
    slots_.push_back(std::make_unique<HashSlot>());
  }
}

std::optional<RedisObject> CacheEngine::Get(std::string_view key,
                                            std::uint64_t now_us) const {
  return SlotForKey(key).Get(key, now_us);
}

WriteResult CacheEngine::Set(std::string_view key, RedisObject obj,
                             BinlogRecord record,
                             std::uint64_t now_us) {
  return SlotForKey(key).Set(key, std::move(obj), std::move(record), now_us);
}

WriteResult CacheEngine::Update(
    std::string_view key,
    std::function<std::optional<RedisObject>(std::optional<RedisObject>)>
        updater,
    BinlogRecord record, std::uint64_t now_us) {
  return SlotForKey(key).Update(key, std::move(updater), std::move(record),
                                now_us);
}

WriteResult CacheEngine::Mutate(
    std::string_view key,
    std::function<MutationResult(std::optional<RedisObject>)> mutator,
    BinlogRecord record, std::uint64_t now_us) {
  return SlotForKey(key).Mutate(key, std::move(mutator), std::move(record),
                                now_us);
}

WriteResult CacheEngine::Del(std::string_view key, std::uint64_t now_us) {
  return SlotForKey(key).Del(key, now_us);
}

std::size_t CacheEngine::DeleteExpiredInSlot(std::size_t slot_id,
                                             std::size_t max_keys,
                                             std::uint64_t now_us) {
  return SlotById(slot_id).DeleteExpired(max_keys, now_us);
}

bool CacheEngine::Expire(std::string_view key, std::int64_t seconds,
                         std::uint64_t now_us) {
  return SlotForKey(key).Expire(key, seconds, now_us);
}

std::int64_t CacheEngine::Ttl(std::string_view key,
                              std::uint64_t now_us) const {
  return SlotForKey(key).Ttl(key, now_us);
}

HashSlot& CacheEngine::SlotForKey(std::string_view key) {
  return SlotById(common::SlotForKey(key));
}

const HashSlot& CacheEngine::SlotForKey(std::string_view key) const {
  return SlotById(common::SlotForKey(key));
}

HashSlot& CacheEngine::SlotById(std::size_t slot_id) {
  return *slots_.at(slot_id);
}

const HashSlot& CacheEngine::SlotById(std::size_t slot_id) const {
  return *slots_.at(slot_id);
}

std::size_t CacheEngine::SlotCount() const { return slots_.size(); }

}  // namespace cache
