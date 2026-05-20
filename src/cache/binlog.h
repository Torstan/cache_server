#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace cache {

enum class BinlogOp {
  kSet,
  kDel,
  kExpire,
  kHSet,
  kSAdd,
  kZAdd,
};

struct BinlogRecord {
  std::uint64_t seq = 0;
  BinlogOp op = BinlogOp::kSet;
  std::vector<std::string> args;
  std::uint64_t remaining_ttl_us = 0;
};

class BinlogBuffer {
 public:
  void Append(BinlogRecord record);
  std::vector<BinlogRecord> CopyAfter(std::uint64_t seq,
                                      std::size_t limit) const;
  void AckThrough(std::uint64_t seq);
  std::uint64_t MinSeq() const;
  std::uint64_t MaxSeq() const;
  bool Empty() const;

 private:
  std::deque<BinlogRecord> records_;
};

}  // namespace cache
