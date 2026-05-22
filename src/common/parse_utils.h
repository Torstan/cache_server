#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace common {

bool ParseInt64(std::string_view text, std::int64_t* out);
bool ParseFiniteDouble(std::string_view text, double* out);
std::string ToUpperAscii(std::string_view text);

}  // namespace common
