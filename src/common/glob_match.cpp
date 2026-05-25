#include "common/glob_match.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace common {
namespace {

bool ParseClassAtom(std::string_view pattern, std::size_t first_member_index,
                    std::size_t* pattern_index, unsigned char* atom,
                    bool* escaped, bool allow_closing_bracket) {
  std::size_t index = *pattern_index;
  if (index >= pattern.size() ||
      (pattern[index] == ']' && index != first_member_index &&
       !allow_closing_bracket)) {
    return false;
  }

  *escaped = false;
  if (pattern[index] == '\\' && index + 1 < pattern.size()) {
    *escaped = true;
    ++index;
  }
  *atom = static_cast<unsigned char>(pattern[index]);
  *pattern_index = index + 1;
  return true;
}

bool MatchClass(std::string_view pattern, std::size_t* pattern_index,
                unsigned char value) {
  std::size_t index = *pattern_index;
  bool negate = false;
  if (index < pattern.size() && pattern[index] == '^') {
    negate = true;
    ++index;
  }

  const std::size_t first_member_index = index;
  bool matched = false;
  bool closed = false;
  while (index < pattern.size()) {
    if (pattern[index] == ']' && index != first_member_index) {
      closed = true;
      ++index;
      break;
    }

    unsigned char first = 0;
    bool first_escaped = false;
    if (!ParseClassAtom(pattern, first_member_index, &index, &first,
                        &first_escaped, false)) {
      break;
    }

    if (!first_escaped && index + 1 < pattern.size() &&
        pattern[index] == '-') {
      std::size_t range_end_index = index + 1;
      unsigned char last = 0;
      bool last_escaped = false;
      if (ParseClassAtom(pattern, first_member_index, &range_end_index, &last,
                         &last_escaped, true)) {
        if (first > last) {
          std::swap(first, last);
        }
        if (first <= value && value <= last) {
          matched = true;
        }
        index = range_end_index;
        if (!last_escaped && last == ']') {
          closed = true;
          break;
        }
        continue;
      }
    }

    if (first == value) {
      matched = true;
    }
  }

  if (!closed) {
    return false;
  }
  *pattern_index = index;
  return negate ? !matched : matched;
}

bool MatchOne(std::string_view pattern, std::size_t* pattern_index,
              unsigned char value) {
  const char token = pattern[*pattern_index];
  if (token == '*') {
    return false;
  }
  std::size_t next_pattern_index = *pattern_index;
  if (token == '?') {
    *pattern_index = next_pattern_index + 1;
    return true;
  }

  if (token == '[') {
    std::size_t range_index = next_pattern_index + 1;
    if (!MatchClass(pattern, &range_index, value)) {
      return false;
    }
    *pattern_index = range_index;
    return true;
  }

  char literal = token;
  if (literal == '\\' && next_pattern_index + 1 < pattern.size()) {
    ++next_pattern_index;
    literal = pattern[next_pattern_index];
  }
  if (static_cast<unsigned char>(literal) != value) {
    return false;
  }
  *pattern_index = next_pattern_index + 1;
  return true;
}

bool MatchAt(std::string_view pattern, std::string_view value) {
  constexpr std::size_t kNoStar = std::numeric_limits<std::size_t>::max();
  std::size_t pattern_index = 0;
  std::size_t value_index = 0;
  std::size_t star_pattern_index = kNoStar;
  std::size_t star_value_index = 0;

  while (value_index < value.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
      }
      star_pattern_index = pattern_index;
      star_value_index = value_index;
      continue;
    }

    if (pattern_index < pattern.size()) {
      std::size_t next_pattern_index = pattern_index;
      if (MatchOne(pattern, &next_pattern_index,
                   static_cast<unsigned char>(value[value_index]))) {
        pattern_index = next_pattern_index;
        ++value_index;
        continue;
      }
    }

    if (star_pattern_index == kNoStar) {
      return false;
    }
    ++star_value_index;
    value_index = star_value_index;
    pattern_index = star_pattern_index;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

}  // namespace

bool GlobMatch(std::string_view pattern, std::string_view value) {
  return MatchAt(pattern, value);
}

}  // namespace common
