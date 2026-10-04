#pragma once

#include <chrono>
#include <cstdint>

namespace common {

inline std::uint64_t NowMicros() {
  // Object deadlines and replication records share a cross-host time domain.
  using clock = std::chrono::system_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          clock::now().time_since_epoch())
          .count());
}

}  // namespace common
