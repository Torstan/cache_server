#include "cache/binlog.h"

#include <utility>

namespace cache {

std::size_t EstimateBinlogRecordBytes(const BinlogRecord& record) {
  std::size_t bytes = sizeof(BinlogRecord);
  for (const std::string& arg : record.args) {
    bytes += sizeof(std::string) + arg.size();
  }
  return bytes;
}

void BinlogBuffer::Append(BinlogRecord record) {
  retained_bytes_ += EstimateBinlogRecordBytes(record);
  records_.push_back(std::move(record));
}

void BinlogBuffer::Clear() {
  records_.clear();
  retained_bytes_ = 0;
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
  (void)AckThroughAndCountBytes(seq);
}

std::size_t BinlogBuffer::AckThroughAndCountBytes(std::uint64_t seq) {
  std::size_t removed = 0;
  while (!records_.empty() && records_.front().seq <= seq) {
    removed += EstimateBinlogRecordBytes(records_.front());
    records_.pop_front();
  }
  retained_bytes_ = removed >= retained_bytes_ ? 0 : retained_bytes_ - removed;
  return removed;
}

std::uint64_t BinlogBuffer::MinSeq() const {
  return records_.empty() ? 0 : records_.front().seq;
}

std::uint64_t BinlogBuffer::MaxSeq() const {
  return records_.empty() ? 0 : records_.back().seq;
}

std::size_t BinlogBuffer::RetainedBytes() const { return retained_bytes_; }

bool BinlogBuffer::Empty() const { return records_.empty(); }

}  // namespace cache
