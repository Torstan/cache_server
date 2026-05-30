#pragma once

#include <cstdint>
#include <string>

#include "cache/cache_engine.h"
#include "repl/master_replicator.h"
#include "repl/slave_replicator.h"

namespace repl {

Frame BuildHelloFrame(const std::string& replica_id,
                      const std::string& previous_session_id,
                      const SlaveReplicator& slave,
                      std::uint64_t proto_version);

bool PollReplicaOnce(const std::string& master_host, std::uint16_t master_port,
                     const std::string& replica_id, SlaveReplicator* slave);

}  // namespace repl
