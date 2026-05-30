#include "repl/replication_link.h"

#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "conn_util/endpoint.h"
#include "protocol/resp_codec.h"
#include "redis/resp.h"
#include "repl/repl_frame.h"

namespace repl {

Frame BuildHelloFrame(const std::string& replica_id,
                      const std::string& previous_session_id,
                      const SlaveReplicator& slave,
                      std::uint64_t proto_version) {
  std::vector<std::pair<std::size_t, std::uint64_t>> positions;
  positions.reserve(slave.SlotCount());
  for (std::size_t slot_id = 0; slot_id < slave.SlotCount(); ++slot_id) {
    positions.push_back({slot_id, slave.AppliedSeqForTest(slot_id)});
  }
  return Frame::Hello(replica_id, proto_version, previous_session_id,
                      std::move(positions));
}

bool PollReplicaOnce(const std::string& master_host, std::uint16_t master_port,
                     const std::string& replica_id, SlaveReplicator* slave) {
  if (slave == nullptr || master_port == 0) {
    return false;
  }

  conn_util::Endpoint endpoint(master_host, master_port);
  sockaddr_in addr;
  if (!endpoint.toSockAddr(&addr)) {
    return false;
  }

  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return false;
  }

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return false;
  }

  std::string hello = EncodeFrame(
      BuildHelloFrame(replica_id, slave->CurrentSessionId(), *slave, 1));
  if (write(fd, hello.data(), hello.size()) !=
      static_cast<ssize_t>(hello.size())) {
    close(fd);
    return false;
  }

  protocol::RespCodec codec(256 * 1024 * 1024, 256 * 1024 * 1024,
                            1'000'000);
  char buffer[64 * 1024];
  bool received_frame = false;
  for (;;) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n == 0) {
      close(fd);
      return received_frame;
    }
    if (n < 0) {
      close(fd);
      return received_frame;
    }
    if (!codec.AppendBytes(
            std::string_view(buffer, static_cast<std::size_t>(n)))) {
      close(fd);
      return false;
    }
    while (auto command = codec.NextCommand()) {
      std::string wire;
      redis::PackArrayHeader(command->args.size(), &wire);
      for (const std::string& arg : command->args) {
        redis::PackBulkString(arg, &wire);
      }
      auto frame = DecodeFrame(wire);
      if (frame.has_value()) {
        if (frame->subcmd == Subcmd::kSnapshot ||
            frame->subcmd == Subcmd::kLog) {
          if (slave->CurrentSessionId().empty()) {
            slave->StartSessionForTest(frame->session_id);
          }
          slave->EnqueueFrame(std::move(*frame));
          received_frame = true;
        }
      }
    }
  }
}

}  // namespace repl
