#include "repl/replication_link.h"

#include <sys/socket.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
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
  return Frame::Hello(replica_id, proto_version, previous_session_id, slave.Positions());
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

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    close(fd);
    return false;
  }
  const int connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (connected != 0) {
    if (errno != EINPROGRESS && errno != EINTR) {
      close(fd);
      return false;
    }
    pollfd pending{fd, POLLOUT, 0};
    int error = 0;
    socklen_t error_size = sizeof(error);
    if (poll(&pending, 1, 5000) <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 ||
        error != 0) {
      close(fd);
      return false;
    }
  }
  if (fcntl(fd, F_SETFL, flags) != 0) {
    close(fd);
    return false;
  }

  std::string hello = EncodeFrame(
      BuildHelloFrame(replica_id, slave->CurrentSessionId(), *slave, kProtocolVersion));
  timeval timeout{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  std::size_t sent = 0;
  while (sent < hello.size()) {
    const auto n = write(fd, hello.data() + sent, hello.size() - sent);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); return false; }
    sent += static_cast<std::size_t>(n);
  }

  protocol::RespCodec codec(256 * 1024 * 1024, 256 * 1024 * 1024,
                            1'000'000);
  char buffer[64 * 1024];
  bool started = false;
  for (;;) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); return false; }
    if (!codec.AppendBytes(
            std::string_view(buffer, static_cast<std::size_t>(n)))) {
      close(fd);
      return false;
    }
    while (auto command = codec.NextCommand()) {
      auto frame = DecodeFrame(command->args);
      if (!frame || (!started && frame->subcmd != Subcmd::kBegin) ||
          (started && frame->subcmd == Subcmd::kBegin)) {
        close(fd);
        return false;
      }
      started = true;
      const bool done = frame->subcmd == Subcmd::kDone;
      if (!slave->EnqueueFrame(std::move(*frame))) { close(fd); return false; }
      if (done) { close(fd); return true; }
    }
    if (codec.HasProtocolError()) { close(fd); return false; }

  }
}

}  // namespace repl
