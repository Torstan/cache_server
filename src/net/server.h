#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "repl/slave_replicator.h"

namespace net {

enum class ServerRole { kMaster, kReplica };

struct ServerConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6399;
  int worker_count = 2;
  int coroutine_count_per_worker = 1024;
  ServerRole role = ServerRole::kMaster;
  bool replica_reads = false;
  std::string master_host = "127.0.0.1";
  std::uint16_t master_port = 0;
  std::string replica_id;
  std::size_t binlog_budget_bytes = 64 * 1024 * 1024;
};

class Server {
 public:
  Server(ServerConfig config, cache::CacheEngine* engine,
         repl::SlaveReplicator* slave_replicator = nullptr);
  int Run();

 private:
  ServerConfig config_;
  cache::CacheEngine* engine_;
  command::CommandDispatcher dispatcher_;
  repl::SlaveReplicator* slave_replicator_ = nullptr;
};

}  // namespace net
