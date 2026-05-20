#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cache/binlog.h"
#include "cache/redis_object.h"

namespace cache {

struct SlotSnapshot {
  ObjectMap map;
  std::uint64_t published_seq = 0;
};

class HashSlot {
 public:
  WriteResult SetString(std::string_view key, std::string_view value,
                        std::uint64_t now_us);
  ReadResult<std::string> GetString(std::string_view key,
                                    std::uint64_t now_us) const;
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
  std::size_t DeleteExpired(std::size_t max_keys, std::uint64_t now_us);
  bool Expire(std::string_view key, std::int64_t seconds,
              std::uint64_t now_us);
  std::int64_t Ttl(std::string_view key, std::uint64_t now_us) const;
  SlotSnapshot Snapshot() const;
  std::vector<BinlogRecord> CopyLogsAfter(std::uint64_t seq,
                                          std::size_t limit) const;
  void AckLogsThrough(std::uint64_t seq);

 private:
  mutable std::mutex write_mutex_;
  mutable std::mutex value_mutex_;
  ObjectMap redis_obj_map_;
  BinlogBuffer binlog_buffer_;
  std::uint64_t slot_seq_ = 0;
  std::uint64_t published_seq_ = 0;
};

}  // namespace cache
