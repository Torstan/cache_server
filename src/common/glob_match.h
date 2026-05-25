#pragma once

#include <string_view>

namespace common {

bool GlobMatch(std::string_view pattern, std::string_view value);

template <typename Emit>
void EmitIfGlobMatches(std::string_view pattern, std::string_view value,
                       Emit&& emit) {
  if (GlobMatch(pattern, value)) {
    emit(value);
  }
}

}  // namespace common
