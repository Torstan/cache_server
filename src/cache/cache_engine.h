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
  WriteResult Mutate(
      std::string_view key,
      std::function<MutationResult(std::optional<RedisObject>)> mutator,
      BinlogRecord record, std::uint64_t now_us);

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
