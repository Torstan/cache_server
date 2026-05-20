#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace common {

static constexpr std::size_t kSlotCount = 100003;

inline std::uint64_t Fnva64(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::size_t SlotForKey(std::string_view key) {
  return static_cast<std::size_t>(Fnva64(key) % kSlotCount);
}

}  // namespace common
