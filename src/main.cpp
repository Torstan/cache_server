#include <cstdlib>
#include <cstdint>
#include <signal.h>

#include "cache/cache_engine.h"
#include "net/server.h"

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

  cache::CacheEngine engine;
  net::Server server(config, &engine);
  return server.Run();
}
