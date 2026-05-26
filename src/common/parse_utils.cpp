#include "common/parse_utils.h"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace common {

bool ParseInt64(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  if (std::isspace(static_cast<unsigned char>(text.front()))) return false;
  std::string str(text);
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(str.c_str(), &end, 10);
  if (errno == ERANGE || end == str.c_str() || *end != '\0') return false;
  *out = static_cast<std::int64_t>(value);
  return true;
}

bool ParseFiniteDouble(std::string_view text, double* out) {
  if (text.empty()) return false;
  std::string str(text);
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(str.c_str(), &end);
  if (errno == ERANGE || end == str.c_str() || *end != '\0' ||
      !std::isfinite(value))
    return false;
  *out = value;
  return true;
}

std::string ToUpperAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    result.push_back(ch >= 'a' && ch <= 'z'
                         ? static_cast<char>(ch - 'a' + 'A')
                         : static_cast<char>(ch));
  }
  return result;
}

}  // namespace common
