#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cache/hash_slot.h"
#include "common/hash.h"

namespace cache {

class CacheEngine {
 public:
  CacheEngine();

  std::optional<RedisObject> Get(std::string_view key,
                                 std::uint64_t now_us) const;
  WriteResult Set(std::string_view key, RedisObject obj, BinlogRecord record,
                  std::uint64_t now_us);
  WriteResult Update(
      std::string_view key,
      std::function<std::optional<RedisObject>(std::optional<RedisObject>)>
          updater,
      BinlogRecord record, std::uint64_t now_us);

  ReadResult<std::string> GetString(std::string_view key,
                                    std::uint64_t now_us) const;
  WriteResult SetString(std::string_view key, std::string_view value,
                        std::uint64_t now_us);
  WriteResult HSet(std::string_view key, std::string_view field,
                   std::string_view value, std::uint64_t now_us);
  ReadResult<std::string> HGet(std::string_view key, std::string_view field,
                               std::uint64_t now_us) const;
  WriteResult SAdd(std::string_view key, std::string_view member,
                   std::uint64_t now_us);
  ReadResult<bool> SIsMember(std::string_view key, std::string_view member,
                             std::uint64_t now_us) const;
  WriteResult ZAdd(std::string_view key, double score, std::string_view member,
                   std::uint64_t now_us);
  ReadResult<double> ZScore(std::string_view key, std::string_view member,
                            std::uint64_t now_us) const;
  WriteResult Del(std::string_view key, std::uint64_t now_us);
  std::size_t DeleteExpiredInSlot(std::size_t slot_id, std::size_t max_keys,
                                  std::uint64_t now_us);
  bool Expire(std::string_view key, std::int64_t seconds,
              std::uint64_t now_us);
  std::int64_t Ttl(std::string_view key, std::uint64_t now_us) const;

  HashSlot& SlotForKey(std::string_view key);
  const HashSlot& SlotForKey(std::string_view key) const;
  HashSlot& SlotById(std::size_t slot_id);
  const HashSlot& SlotById(std::size_t slot_id) const;
  std::size_t SlotCount() const;

 private:
  // Keep slots heap-owned so a stack-allocated CacheEngine does not reserve
  // space for 100003 mutex-bearing HashSlot instances.
  std::vector<std::unique_ptr<HashSlot>> slots_;
};

}  // namespace cache
