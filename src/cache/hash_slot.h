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
  WriteResult Del(std::string_view key, std::uint64_t now_us);
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
