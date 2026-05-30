#include "repl/repl_frame.h"

#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <utility>

#include "redis/resp.h"

namespace repl {
namespace {

constexpr std::size_t kMaxFrameElements = 1'000'000;

void PackBulk(std::string_view text, std::string* out) {
  redis::PackBulkString(text, out);
}

void PackNumber(std::uint64_t value, std::string* out) {
  PackBulk(std::to_string(value), out);
}

void PackSize(std::size_t value, std::string* out) {
  PackBulk(std::to_string(value), out);
}

bool IsBulk(const redis::RespValue& value) {
  return value.type == redis::RespType::kBulkString;
}

std::optional<std::uint64_t> ParseU64(std::string_view text) {
  std::uint64_t value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::size_t> ParseSize(std::string_view text) {
  const auto value = ParseU64(text);
  if (!value.has_value() ||
      *value > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(*value);
}

bool AllBulk(const redis::RespValue& value) {
  for (std::size_t i = 0; i < value.element_count; ++i) {
    if (!IsBulk(value.elements[i])) {
      return false;
    }
  }
  return true;
}

std::optional<Frame> DecodeHello(const redis::RespValue& value) {
  if (value.element_count < 6) {
    return std::nullopt;
  }
  if (!AllBulk(value)) {
    return std::nullopt;
  }
  auto proto = ParseU64(value.elements[3].text);
  auto count = ParseSize(value.elements[5].text);
  if (!proto.has_value() || !count.has_value()) {
    return std::nullopt;
  }
  if (value.element_count != 6 + *count * 2) {
    return std::nullopt;
  }

  std::vector<std::pair<std::size_t, std::uint64_t>> slots;
  slots.reserve(*count);
  for (std::size_t i = 0; i < *count; ++i) {
    auto slot_id = ParseSize(value.elements[6 + i * 2].text);
    auto seq = ParseU64(value.elements[7 + i * 2].text);
    if (!slot_id.has_value() || !seq.has_value()) {
      return std::nullopt;
    }
    slots.push_back({*slot_id, *seq});
  }

  Frame frame;
  frame.subcmd = Subcmd::kHello;
  frame.replica_id.assign(value.elements[2].text.data(),
                          value.elements[2].text.size());
  frame.proto_version = *proto;
  frame.session_id.assign(value.elements[4].text.data(),
                          value.elements[4].text.size());
  frame.slot_positions = std::move(slots);
  return frame;
}

std::optional<Frame> DecodeSnapshot(const redis::RespValue& value) {
  if (value.element_count != 6) {
    return std::nullopt;
  }
  if (!AllBulk(value)) {
    return std::nullopt;
  }
  auto slot_id = ParseSize(value.elements[3].text);
  auto base_seq = ParseU64(value.elements[4].text);
  if (!slot_id.has_value() || !base_seq.has_value()) {
    return std::nullopt;
  }
  Frame frame;
  frame.subcmd = Subcmd::kSnapshot;
  frame.session_id.assign(value.elements[2].text.data(),
                          value.elements[2].text.size());
  frame.slot_id = *slot_id;
  frame.base_seq = *base_seq;
  frame.snapshot_payload.assign(value.elements[5].text.data(),
                                value.elements[5].text.size());
  return frame;
}

std::optional<Frame> DecodeLog(const redis::RespValue& value) {
  if (value.element_count < 8) {
    return std::nullopt;
  }
  if (!AllBulk(value)) {
    return std::nullopt;
  }

  auto slot_id = ParseSize(value.elements[3].text);
  auto seq = ParseU64(value.elements[4].text);
  auto ttl = ParseU64(value.elements[6].text);
  auto arg_count = ParseSize(value.elements[7].text);
  if (!slot_id.has_value() || !seq.has_value() || !ttl.has_value() ||
      !arg_count.has_value()) {
    return std::nullopt;
  }
  if (value.element_count != 8 + *arg_count) {
    return std::nullopt;
  }
  if (*arg_count > 0 && value.elements[5].text != value.elements[8].text) {
    return std::nullopt;
  }

  cache::BinlogRecord record;
  record.seq = *seq;
  record.remaining_ttl_us = *ttl;
  record.args.reserve(*arg_count);
  for (std::size_t i = 0; i < *arg_count; ++i) {
    const std::string_view arg = value.elements[8 + i].text;
    record.args.emplace_back(arg.data(), arg.size());
  }

  Frame frame;
  frame.subcmd = Subcmd::kLog;
  frame.session_id.assign(value.elements[2].text.data(),
                          value.elements[2].text.size());
  frame.slot_id = *slot_id;
  frame.record = std::move(record);
  return frame;
}

std::optional<Frame> DecodeAck(const redis::RespValue& value) {
  if (value.element_count < 4 || ((value.element_count - 4) % 2) != 0) {
    return std::nullopt;
  }
  if (!AllBulk(value)) {
    return std::nullopt;
  }
  auto count = ParseSize(value.elements[3].text);
  if (!count.has_value() || value.element_count != 4 + *count * 2) {
    return std::nullopt;
  }

  std::vector<std::pair<std::size_t, std::uint64_t>> slots;
  slots.reserve(*count);
  for (std::size_t i = 0; i < *count; ++i) {
    auto slot_id = ParseSize(value.elements[4 + i * 2].text);
    auto seq = ParseU64(value.elements[5 + i * 2].text);
    if (!slot_id.has_value() || !seq.has_value()) {
      return std::nullopt;
    }
    slots.push_back({*slot_id, *seq});
  }

  Frame frame;
  frame.subcmd = Subcmd::kAck;
  frame.session_id.assign(value.elements[2].text.data(),
                          value.elements[2].text.size());
  frame.acked_slots = std::move(slots);
  return frame;
}

}  // namespace

Frame Frame::Hello(std::string replica_id, std::uint64_t proto_version,
                   std::string previous_session_id,
                   std::vector<std::pair<std::size_t, std::uint64_t>> slots) {
  Frame frame;
  frame.subcmd = Subcmd::kHello;
  frame.replica_id = std::move(replica_id);
  frame.proto_version = proto_version;
  frame.session_id = std::move(previous_session_id);
  frame.slot_positions = std::move(slots);
  return frame;
}

Frame Frame::Snapshot(std::string session_id, std::size_t slot_id,
                      std::uint64_t base_seq, std::string payload) {
  Frame frame;
  frame.subcmd = Subcmd::kSnapshot;
  frame.session_id = std::move(session_id);
  frame.slot_id = slot_id;
  frame.base_seq = base_seq;
  frame.snapshot_payload = std::move(payload);
  return frame;
}

Frame Frame::Log(std::string session_id, std::size_t slot_id,
                 cache::BinlogRecord record) {
  Frame frame;
  frame.subcmd = Subcmd::kLog;
  frame.session_id = std::move(session_id);
  frame.slot_id = slot_id;
  frame.record = std::move(record);
  return frame;
}

Frame Frame::Ack(std::string session_id,
                 std::vector<std::pair<std::size_t, std::uint64_t>> slots) {
  Frame frame;
  frame.subcmd = Subcmd::kAck;
  frame.session_id = std::move(session_id);
  frame.acked_slots = std::move(slots);
  return frame;
}

std::string EncodeFrame(const Frame& frame) {
  std::string out;
  switch (frame.subcmd) {
    case Subcmd::kHello: {
      const std::size_t slots = frame.slot_positions.size();
      redis::PackArrayHeader(6 + slots * 2, &out);
      PackBulk("CACHE.REPL", &out);
      PackBulk("HELLO", &out);
      PackBulk(frame.replica_id, &out);
      PackNumber(frame.proto_version, &out);
      PackBulk(frame.session_id, &out);
      PackSize(slots, &out);
      for (const auto& slot : frame.slot_positions) {
        PackSize(slot.first, &out);
        PackNumber(slot.second, &out);
      }
      break;
    }
    case Subcmd::kSnapshot:
      redis::PackArrayHeader(6, &out);
      PackBulk("CACHE.REPL", &out);
      PackBulk("SNAPSHOT", &out);
      PackBulk(frame.session_id, &out);
      PackSize(frame.slot_id, &out);
      PackNumber(frame.base_seq, &out);
      PackBulk(frame.snapshot_payload, &out);
      break;
    case Subcmd::kLog:
      redis::PackArrayHeader(8 + frame.record.args.size(), &out);
      PackBulk("CACHE.REPL", &out);
      PackBulk("LOG", &out);
      PackBulk(frame.session_id, &out);
      PackSize(frame.slot_id, &out);
      PackNumber(frame.record.seq, &out);
      PackBulk(frame.record.args.empty() ? "" : frame.record.args[0], &out);
      PackNumber(frame.record.remaining_ttl_us, &out);
      PackSize(frame.record.args.size(), &out);
      for (const std::string& arg : frame.record.args) {
        PackBulk(arg, &out);
      }
      break;
    case Subcmd::kAck:
      redis::PackArrayHeader(4 + frame.acked_slots.size() * 2, &out);
      PackBulk("CACHE.REPL", &out);
      PackBulk("ACK", &out);
      PackBulk(frame.session_id, &out);
      PackSize(frame.acked_slots.size(), &out);
      for (const auto& slot : frame.acked_slots) {
        PackSize(slot.first, &out);
        PackNumber(slot.second, &out);
      }
      break;
  }
  return out;
}

std::optional<Frame> DecodeFrame(std::string_view wire) {
  std::vector<redis::RespValue> scratch(kMaxFrameElements);
  redis::RespLimits limits;
  limits.max_array_elements = kMaxFrameElements;
  limits.max_bulk_bytes = wire.size();
  redis::RespResult result =
      redis::UnpackOne(wire, scratch.data(), scratch.size(), limits);
  if (result.status != redis::RespStatus::kOk ||
      result.consumed != wire.size() || result.value == nullptr ||
      result.value->type != redis::RespType::kArray ||
      result.value->element_count < 2) {
    return std::nullopt;
  }

  const redis::RespValue& value = *result.value;
  if (!IsBulk(value.elements[0]) || !IsBulk(value.elements[1]) ||
      value.elements[0].text != "CACHE.REPL") {
    return std::nullopt;
  }

  const std::string_view subcmd = value.elements[1].text;
  if (subcmd == "HELLO") {
    return DecodeHello(value);
  }
  if (subcmd == "SNAPSHOT") {
    return DecodeSnapshot(value);
  }
  if (subcmd == "LOG") {
    return DecodeLog(value);
  }
  if (subcmd == "ACK") {
    return DecodeAck(value);
  }
  return std::nullopt;
}

}  // namespace repl
