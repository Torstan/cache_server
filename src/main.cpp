#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <signal.h>
#include <unistd.h>

#include "cache/cache_engine.h"
#include "conn_util/endpoint.h"
#include "net/server.h"
#include "repl/master_replicator.h"
#include "repl/replication_link.h"
#include "repl/slave_replicator.h"

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);

  net::ServerConfig config;
  if (argc >= 2) {
    config.port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  }
  if (argc >= 3) {
    config.worker_count = std::atoi(argv[2]);
  }
  if (argc >= 4) {
    config.coroutine_count_per_worker = std::atoi(argv[3]);
  }

  for (int i = 4; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--replica") {
      config.role = net::ServerRole::kReplica;
    } else if (arg == "--replica-reads") {
      config.replica_reads = true;
    } else if (arg.rfind("--master=", 0) == 0) {
      std::string value(arg.substr(9));
      conn_util::Endpoint endpoint;
      if (conn_util::Endpoint::parse(value, &endpoint)) {
        config.master_host = endpoint.host();
        config.master_port = endpoint.port();
      }
    } else if (arg.rfind("--replica-id=", 0) == 0) {
      config.replica_id = std::string(arg.substr(13));
    }
  }

  cache::CacheEngine engine;
  std::unique_ptr<repl::SlaveReplicator> slave;
  if (config.role == net::ServerRole::kReplica) {
    slave = std::make_unique<repl::SlaveReplicator>(&engine,
                                                    config.worker_count);
  }
  std::unique_ptr<repl::MasterReplicator> master;
  if (config.role == net::ServerRole::kMaster) {
    master = std::make_unique<repl::MasterReplicator>(&engine);
  }

  if (config.role == net::ServerRole::kReplica && slave != nullptr) {
    std::thread([&config, slave_ptr = slave.get()]() {
      for (;;) {
        repl::PollReplicaOnce(config.master_host, config.master_port,
                              config.replica_id, slave_ptr);
        sleep(1);
      }
    }).detach();
  }

  net::Server server(config, &engine, slave.get(), master.get());
  return server.Run();
}
