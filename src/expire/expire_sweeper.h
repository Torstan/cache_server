#pragma once

#include <cstddef>
#include <cstdint>

#include "cache/cache_engine.h"

namespace expire {

class ExpireSweeper {
 public:
  ExpireSweeper(cache::CacheEngine* engine, std::size_t slots_per_tick);
  std::size_t SweepOnce(std::uint64_t now_us);

 private:
  cache::CacheEngine* engine_;
  std::size_t slots_per_tick_;
  std::size_t next_slot_ = 0;
};

}  // namespace expire
