#include "cache/cache_engine.h"

namespace cache {

CacheEngine::CacheEngine() {
  slots_.reserve(common::kSlotCount);
  for (std::size_t i = 0; i < common::kSlotCount; ++i) {
    slots_.push_back(std::make_unique<HashSlot>());
  }
}

ReadResult<std::string> CacheEngine::GetString(std::string_view key,
                                               std::uint64_t now_us) const {
  return SlotForKey(key).GetString(key, now_us);
}

WriteResult CacheEngine::SetString(std::string_view key,
                                   std::string_view value,
                                   std::uint64_t now_us) {
  return SlotForKey(key).SetString(key, value, now_us);
}

WriteResult CacheEngine::HSet(std::string_view key, std::string_view field,
                              std::string_view value, std::uint64_t now_us) {
  return SlotForKey(key).HSet(key, field, value, now_us);
}

ReadResult<std::string> CacheEngine::HGet(std::string_view key,
                                          std::string_view field,
                                          std::uint64_t now_us) const {
  return SlotForKey(key).HGet(key, field, now_us);
}

WriteResult CacheEngine::SAdd(std::string_view key, std::string_view member,
                              std::uint64_t now_us) {
  return SlotForKey(key).SAdd(key, member, now_us);
}

ReadResult<bool> CacheEngine::SIsMember(std::string_view key,
                                        std::string_view member,
                                        std::uint64_t now_us) const {
  return SlotForKey(key).SIsMember(key, member, now_us);
}

WriteResult CacheEngine::ZAdd(std::string_view key, double score,
                              std::string_view member, std::uint64_t now_us) {
  return SlotForKey(key).ZAdd(key, score, member, now_us);
}

ReadResult<double> CacheEngine::ZScore(std::string_view key,
                                       std::string_view member,
                                       std::uint64_t now_us) const {
  return SlotForKey(key).ZScore(key, member, now_us);
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
