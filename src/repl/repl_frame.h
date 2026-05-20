#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cache/binlog.h"

namespace repl {

enum class Subcmd { kHello, kSnap, kSnapCommit, kLog, kAck, kOnline };

struct Frame {
  Subcmd subcmd = Subcmd::kHello;
  std::string repl_id;
  std::size_t slot_id = 0;
  std::uint64_t chunk_id = 0;
  std::uint64_t base_seq = 0;
  std::uint64_t end_seq = 0;
  cache::BinlogRecord record;
  std::vector<std::pair<std::size_t, std::uint64_t>> acked_slots;

  static Frame Log(std::size_t slot_id, cache::BinlogRecord record);
  static Frame Ack(std::vector<std::pair<std::size_t, std::uint64_t>> slots);
};

std::string EncodeFrame(const Frame& frame);
std::optional<Frame> DecodeFrame(std::string_view wire);

}  // namespace repl
