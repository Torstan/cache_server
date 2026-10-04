#include "repl/repl_frame.h"

#include <charconv>
#include <limits>
#include <utility>

#include "protocol/resp_codec.h"
#include "redis/resp.h"

namespace repl {
namespace {
constexpr std::size_t kMaxFrameElements = 1'000'000;

bool Number(std::string_view text, std::uint64_t* value) {
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

bool Positions(const std::vector<std::string>& args, std::size_t start,
               std::vector<std::pair<std::size_t, std::uint64_t>>* out) {
  std::uint64_t count = 0;
  if (start >= args.size() || !Number(args[start], &count) ||
      (args.size() - start - 1) % 2 != 0 ||
      count != (args.size() - start - 1) / 2) return false;
  out->reserve(static_cast<std::size_t>(count));
  for (std::size_t i = start + 1; i < args.size(); i += 2) {
    std::uint64_t slot = 0, seq = 0;
    if (!Number(args[i], &slot) || !Number(args[i + 1], &seq) ||
        slot > std::numeric_limits<std::size_t>::max()) return false;
    out->emplace_back(static_cast<std::size_t>(slot), seq);
  }
  return true;
}
}

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

Frame Frame::Snapshot(std::string session, std::size_t slot,
                      std::uint64_t seq, std::string payload) {
  Frame frame;
  frame.subcmd = Subcmd::kSnapshot;
  frame.session_id = std::move(session);
  frame.slot_id = slot;
  frame.base_seq = seq;
  frame.snapshot_payload = std::move(payload);
  return frame;
}

Frame Frame::Log(std::string session, std::size_t slot, cache::BinlogRecord record) {
  Frame frame;
  frame.subcmd = Subcmd::kLog;
  frame.session_id = std::move(session);
  frame.slot_id = slot;
  frame.record = std::move(record);
  return frame;
}


Frame Frame::Begin(std::string session, bool reset) {
  Frame frame;
  frame.subcmd = Subcmd::kBegin;
  frame.session_id = std::move(session);
  frame.reset = reset;
  return frame;
}

Frame Frame::Done(std::string session,
                  std::vector<std::pair<std::size_t, std::uint64_t>> slots) {
  Frame frame;
  frame.subcmd = Subcmd::kDone;
  frame.session_id = std::move(session);
  frame.slot_positions = std::move(slots);
  return frame;
}

std::string EncodeFrame(const Frame& frame) {
  std::vector<std::string> args{"CACHE.REPL"};
  auto number = [&](std::uint64_t value) { args.push_back(std::to_string(value)); };
  auto positions = [&](const auto& slots) {
    number(slots.size());
    for (const auto& item : slots) { number(item.first); number(item.second); }
  };
  switch (frame.subcmd) {
    case Subcmd::kHello:
      args.insert(args.end(), {"HELLO", frame.replica_id});
      number(frame.proto_version);
      args.push_back(frame.session_id);
      positions(frame.slot_positions);
      break;
    case Subcmd::kSnapshot:
      args.insert(args.end(), {"SNAPSHOT", frame.session_id});
      number(frame.slot_id);
      number(frame.base_seq);
      args.push_back(frame.snapshot_payload);
      break;
    case Subcmd::kLog:
      args.insert(args.end(), {"LOG", frame.session_id});
      number(frame.slot_id);
      number(frame.record.seq);
      number(frame.record.written_at_us);
      number(frame.record.deadline_us);
      number(frame.record.args.size());
      args.insert(args.end(), frame.record.args.begin(), frame.record.args.end());
      break;
    case Subcmd::kBegin:
      args.insert(args.end(), {"BEGIN", frame.session_id, frame.reset ? "1" : "0"});
      break;
    case Subcmd::kDone:
      args.insert(args.end(), {"DONE", frame.session_id});
      positions(frame.slot_positions);
      break;
  }
  std::string out;
  redis::PackArrayHeader(args.size(), &out);
  for (const auto& arg : args) redis::PackBulkString(arg, &out);
  return out;
}

std::optional<Frame> DecodeFrame(const std::vector<std::string>& args) {
  if (args.size() < 3 || args[0] != "CACHE.REPL") return std::nullopt;
  Frame frame;
  frame.session_id = args[2];
  if (args[1] == "HELLO") {
    if (args.size() < 6 || !Number(args[3], &frame.proto_version) ||
        frame.proto_version != kProtocolVersion || !Positions(args, 5, &frame.slot_positions))
      return std::nullopt;
    frame.subcmd = Subcmd::kHello;
    frame.replica_id = args[2];
    frame.session_id = args[4];
  } else if (args[1] == "SNAPSHOT") {
    std::uint64_t slot = 0;
    if (args.size() != 6 || !Number(args[3], &slot) ||
        slot > std::numeric_limits<std::size_t>::max() || !Number(args[4], &frame.base_seq))
      return std::nullopt;
    frame.subcmd = Subcmd::kSnapshot;
    frame.slot_id = static_cast<std::size_t>(slot);
    frame.snapshot_payload = args[5];
  } else if (args[1] == "LOG") {
    std::uint64_t slot = 0, count = 0;
    if (args.size() < 8 || !Number(args[3], &slot) ||
        slot > std::numeric_limits<std::size_t>::max() ||
        !Number(args[4], &frame.record.seq) || !Number(args[5], &frame.record.written_at_us) ||
        !Number(args[6], &frame.record.deadline_us) || !Number(args[7], &count) ||
        count != args.size() - 8) return std::nullopt;
    frame.subcmd = Subcmd::kLog;
    frame.slot_id = static_cast<std::size_t>(slot);
    frame.record.args.assign(args.begin() + 8, args.end());
  } else if (args[1] == "BEGIN") {
    if (args.size() != 4 || (args[3] != "0" && args[3] != "1") || args[2].empty())
      return std::nullopt;
    frame.subcmd = Subcmd::kBegin;
    frame.reset = args[3] == "1";
  } else if (args[1] == "DONE") {
    frame.subcmd = Subcmd::kDone;
    if (!Positions(args, 3, &frame.slot_positions)) return std::nullopt;
  } else {
    return std::nullopt;
  }
  return frame;
}

std::optional<Frame> DecodeFrame(std::string_view wire) {
  protocol::RespCodec codec(wire.size(), wire.size(), kMaxFrameElements);
  if (!codec.AppendBytes(wire)) return std::nullopt;
  auto args = codec.NextCommand();
  if (!args || codec.BufferedBytes() != 0) return std::nullopt;
  return DecodeFrame(args->args);
}

}  // namespace repl
