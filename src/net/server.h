#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"

namespace net {

struct ServerConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6399;
  int worker_count = 2;
  int coroutine_count_per_worker = 1024;
};

class Server {
 public:
  Server(ServerConfig config, cache::CacheEngine* engine);
  int Run();

 private:
  ServerConfig config_;
  cache::CacheEngine* engine_;
  command::CommandDispatcher dispatcher_;
};

}  // namespace net
