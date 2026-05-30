#pragma once

#include <cstdint>
#include <deque>
#include <optional>
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
  std::optional<BinlogOp> op;
  std::vector<std::string> args;
  std::uint64_t remaining_ttl_us = 0;
};

std::size_t EstimateBinlogRecordBytes(const BinlogRecord& record);

class BinlogBuffer {
 public:
  void Append(BinlogRecord record);
  std::vector<BinlogRecord> CopyAfter(std::uint64_t seq,
                                      std::size_t limit) const;
  void AckThrough(std::uint64_t seq);
  std::size_t AckThroughAndCountBytes(std::uint64_t seq);
  void Clear();
  std::uint64_t MinSeq() const;
  std::uint64_t MaxSeq() const;
  std::size_t RetainedBytes() const;
  bool Empty() const;

 private:
  std::deque<BinlogRecord> records_;
  std::size_t retained_bytes_ = 0;
};

}  // namespace cache
