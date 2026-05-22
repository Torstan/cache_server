#include "repl/repl_frame.h"

#include <array>
#include <charconv>
#include <limits>
#include <string>

#include "redis/resp.h"

namespace repl {
namespace {

constexpr std::size_t kMaxFrameElements = 1024;

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

std::optional<Frame> DecodeLog(const redis::RespValue& value) {
  if (value.element_count < 7) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < value.element_count; ++i) {
    if (!IsBulk(value.elements[i])) {
      return std::nullopt;
    }
  }

  auto slot_id = ParseSize(value.elements[2].text);
  auto seq = ParseU64(value.elements[3].text);
  auto ttl = ParseU64(value.elements[5].text);
  auto arg_count = ParseSize(value.elements[6].text);
  if (!slot_id.has_value() || !seq.has_value() || !ttl.has_value() ||
      !arg_count.has_value()) {
    return std::nullopt;
  }
  if (*arg_count > value.element_count - 7) {
    return std::nullopt;
  }
  if (value.element_count != 7 + *arg_count) {
    return std::nullopt;
  }
  if (*arg_count > 0 && value.elements[4].text != value.elements[7].text) {
    return std::nullopt;
  }

  cache::BinlogRecord record;
  record.seq = *seq;
  record.remaining_ttl_us = *ttl;
  record.args.reserve(*arg_count);
  for (std::size_t i = 0; i < *arg_count; ++i) {
    const std::string_view arg = value.elements[7 + i].text;
    record.args.emplace_back(arg.data(), arg.size());
  }
  return Frame::Log(*slot_id, std::move(record));
}

std::optional<Frame> DecodeAck(const redis::RespValue& value) {
  if (value.element_count < 3 || ((value.element_count - 3) % 2) != 0) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < value.element_count; ++i) {
    if (!IsBulk(value.elements[i])) {
      return std::nullopt;
    }
  }

  auto count = ParseSize(value.elements[2].text);
  if (!count.has_value() || *count > (value.element_count - 3) / 2) {
    return std::nullopt;
  }
  if (value.element_count != 3 + *count * 2) {
    return std::nullopt;
  }

  std::vector<std::pair<std::size_t, std::uint64_t>> slots;
  slots.reserve(*count);
  for (std::size_t i = 0; i < *count; ++i) {
    auto slot_id = ParseSize(value.elements[3 + i * 2].text);
    auto seq = ParseU64(value.elements[4 + i * 2].text);
    if (!slot_id.has_value() || !seq.has_value()) {
      return std::nullopt;
    }
    slots.push_back({*slot_id, *seq});
  }
  return Frame::Ack(std::move(slots));
}

}  // namespace

Frame Frame::Log(std::size_t slot_id, cache::BinlogRecord record) {
  Frame frame;
  frame.subcmd = Subcmd::kLog;
  frame.slot_id = slot_id;
  frame.record = std::move(record);
  return frame;
}

Frame Frame::Ack(std::vector<std::pair<std::size_t, std::uint64_t>> slots) {
  Frame frame;
  frame.subcmd = Subcmd::kAck;
  frame.acked_slots = std::move(slots);
  return frame;
}

std::string EncodeFrame(const Frame& frame) {
  std::string out;
  switch (frame.subcmd) {
    case Subcmd::kLog:
      redis::PackArrayHeader(7 + frame.record.args.size(), &out);
      PackBulk("CACHE.REPL", &out);
      PackBulk("LOG", &out);
      PackSize(frame.slot_id, &out);
      PackNumber(frame.record.seq, &out);
      PackBulk(frame.record.args.empty() ? "SET" : frame.record.args[0], &out);
      PackNumber(frame.record.remaining_ttl_us, &out);
      PackSize(frame.record.args.size(), &out);
      for (const std::string& arg : frame.record.args) {
        PackBulk(arg, &out);
      }
      break;
    case Subcmd::kAck:
      redis::PackArrayHeader(3 + frame.acked_slots.size() * 2, &out);
      PackBulk("CACHE.REPL", &out);
      PackBulk("ACK", &out);
      PackSize(frame.acked_slots.size(), &out);
      for (const auto& slot : frame.acked_slots) {
        PackSize(slot.first, &out);
        PackNumber(slot.second, &out);
      }
      break;
    case Subcmd::kHello:
      redis::PackCommand({"CACHE.REPL", "HELLO", frame.repl_id}, &out);
      break;
    case Subcmd::kSnap:
      redis::PackCommand({"CACHE.REPL", "SNAP"}, &out);
      break;
    case Subcmd::kSnapCommit:
      redis::PackCommand({"CACHE.REPL", "SNAP_COMMIT"}, &out);
      break;
    case Subcmd::kOnline:
      redis::PackCommand({"CACHE.REPL", "ONLINE"}, &out);
      break;
  }
  return out;
}

std::optional<Frame> DecodeFrame(std::string_view wire) {
  std::array<redis::RespValue, kMaxFrameElements> scratch{};
  redis::RespLimits limits;
  limits.max_array_elements = kMaxFrameElements;
  redis::RespResult result =
      redis::UnpackOne(wire, scratch.data(), scratch.size(), limits);
  if (result.status != redis::RespStatus::kOk || result.consumed != wire.size() ||
      result.value == nullptr || result.value->type != redis::RespType::kArray ||
      result.value->element_count < 2) {
    return std::nullopt;
  }

  const redis::RespValue& value = *result.value;
  if (!IsBulk(value.elements[0]) || !IsBulk(value.elements[1]) ||
      value.elements[0].text != "CACHE.REPL") {
    return std::nullopt;
  }

  const std::string_view subcmd = value.elements[1].text;
  if (subcmd == "LOG") {
    return DecodeLog(value);
  }
  if (subcmd == "ACK") {
    return DecodeAck(value);
  }
  if (subcmd == "HELLO") {
    Frame frame;
    frame.subcmd = Subcmd::kHello;
    if (value.element_count >= 3 && IsBulk(value.elements[2])) {
      frame.repl_id.assign(value.elements[2].text.data(),
                           value.elements[2].text.size());
    }
    return frame;
  }
  if (subcmd == "SNAP") {
    Frame frame;
    frame.subcmd = Subcmd::kSnap;
    return frame;
  }
  if (subcmd == "SNAP_COMMIT") {
    Frame frame;
    frame.subcmd = Subcmd::kSnapCommit;
    return frame;
  }
  if (subcmd == "ONLINE") {
    Frame frame;
    frame.subcmd = Subcmd::kOnline;
    return frame;
  }
  return std::nullopt;
}

}  // namespace repl
