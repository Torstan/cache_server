#include "cache/binlog.h"

#include <utility>

namespace cache {

void BinlogBuffer::Append(BinlogRecord record) {
  records_.push_back(std::move(record));
}

std::vector<BinlogRecord> BinlogBuffer::CopyAfter(std::uint64_t seq,
                                                  std::size_t limit) const {
  std::vector<BinlogRecord> result;
  if (limit == 0) {
    return result;
  }
  result.reserve(limit);
  for (const BinlogRecord& record : records_) {
    if (record.seq > seq) {
      result.push_back(record);
      if (result.size() == limit) {
        break;
      }
    }
  }
  return result;
}

void BinlogBuffer::AckThrough(std::uint64_t seq) {
  while (!records_.empty() && records_.front().seq <= seq) {
    records_.pop_front();
  }
}

std::uint64_t BinlogBuffer::MinSeq() const {
  return records_.empty() ? 0 : records_.front().seq;
}

std::uint64_t BinlogBuffer::MaxSeq() const {
  return records_.empty() ? 0 : records_.back().seq;
}

bool BinlogBuffer::Empty() const { return records_.empty(); }

}  // namespace cache
