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

enum class Subcmd { kHello, kSnapshot, kLog, kAck };

struct Frame {
  Subcmd subcmd = Subcmd::kHello;
  std::string replica_id;
  std::string session_id;
  std::uint64_t proto_version = 1;
  std::size_t slot_id = 0;
  std::uint64_t base_seq = 0;
  std::string snapshot_payload;
  cache::BinlogRecord record;
  std::vector<std::pair<std::size_t, std::uint64_t>> slot_positions;
  std::vector<std::pair<std::size_t, std::uint64_t>> acked_slots;

  static Frame Hello(
      std::string replica_id, std::uint64_t proto_version,
      std::string previous_session_id,
      std::vector<std::pair<std::size_t, std::uint64_t>> slots);
  static Frame Snapshot(std::string session_id, std::size_t slot_id,
                        std::uint64_t base_seq, std::string payload);
  static Frame Log(std::string session_id, std::size_t slot_id,
                   cache::BinlogRecord record);
  static Frame Ack(std::string session_id,
                   std::vector<std::pair<std::size_t, std::uint64_t>> slots);
};

std::string EncodeFrame(const Frame& frame);
std::optional<Frame> DecodeFrame(std::string_view wire);

}  // namespace repl
