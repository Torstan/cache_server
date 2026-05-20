#pragma once

#include <chrono>
#include <cstdint>

namespace common {

inline std::uint64_t NowMicros() {
  using clock = std::chrono::steady_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          clock::now().time_since_epoch())
          .count());
}

}  // namespace common
