#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace protocol {

enum class ResponseType {
  kSimpleString,
  kError,
  kInteger,
  kBulkString,
  kNullBulkString,
  kArray,
};

struct Response {
  ResponseType type = ResponseType::kNullBulkString;
  std::string text;
  std::int64_t integer = 0;
  std::vector<Response> elements;

  static Response SimpleString(std::string value);
  static Response Error(std::string value);
  static Response Integer(std::int64_t value);
  static Response BulkString(std::string value);
  static Response BulkString(const char* value);
  static Response BulkString(std::string_view value);
  static Response NullBulk();
  static Response Array(std::vector<Response> values);
};

}  // namespace protocol
