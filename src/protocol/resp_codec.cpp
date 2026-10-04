#include "protocol/resp_codec.h"

#include <utility>

#include "common/parse_utils.h"

namespace protocol {

namespace {

void PackResponseElements(const std::vector<Response>& elements,
                          std::string* out) {
  redis::PackArrayHeader(elements.size(), out);
  for (const Response& element : elements) {
    PackResponse(element, out);
  }
}

}  // namespace

Response Response::SimpleString(std::string value) {
  Response response;
  response.type = ResponseType::kSimpleString;
  response.text = std::move(value);
  return response;
}

Response Response::Error(std::string value) {
  Response response;
  response.type = ResponseType::kError;
  response.text = std::move(value);
  return response;
}

Response Response::Integer(std::int64_t value) {
  Response response;
  response.type = ResponseType::kInteger;
  response.integer = value;
  return response;
}

Response Response::BulkString(std::string value) {
  Response response;
  response.type = ResponseType::kBulkString;
  response.text = std::move(value);
  return response;
}

Response Response::BulkString(const char* value) {
  return BulkString(std::string_view(value));
}

Response Response::BulkString(std::string_view value) {
  Response response;
  response.type = ResponseType::kBulkString;
  response.text.assign(value.data(), value.size());
  return response;
}

Response Response::NullBulk() {
  Response response;
  response.type = ResponseType::kNullBulkString;
  return response;
}

Response Response::Array(std::vector<Response> values) {
  Response response;
  response.type = ResponseType::kArray;
  response.elements = std::move(values);
  return response;
}

RespCodec::RespCodec(std::size_t max_stream_bytes, std::size_t max_bulk_bytes,
                     std::size_t max_array_elements)
    : max_stream_bytes_(max_stream_bytes) {
  limits_.max_bulk_bytes = max_bulk_bytes;
  limits_.max_array_elements = max_array_elements;
  limits_.max_depth = 1;
}

bool RespCodec::AppendBytes(std::string_view bytes) {
  if (protocol_error_) {
    return false;
  }
  if (bytes.size() > max_stream_bytes_ ||
      stream_.size() > max_stream_bytes_ - bytes.size()) {
    protocol_error_ = true;
    protocol_error_text_ = "RESP stream size limit exceeded";
    return false;
  }
  stream_.append(bytes.data(), bytes.size());
  return true;
}

std::optional<CommandArgs> RespCodec::NextCommand(
    const std::function<bool(std::string_view, std::size_t)>& accept) {
  if (protocol_error_) {
    return std::nullopt;
  }

  if (stream_.empty()) return std::nullopt;
  const auto line_end = stream_.find("\r\n");
  if (line_end == std::string::npos) return std::nullopt;
  std::int64_t count = 0;
  if (stream_[0] != '*' ||
      !common::ParseInt64(std::string_view(stream_).substr(1, line_end - 1), &count) ||
      count < 0 || static_cast<std::uint64_t>(count) > limits_.max_array_elements) {
    protocol_error_ = true;
    protocol_error_text_ = "invalid command array length";
    return std::nullopt;
  }
  if (accept && count > 0) {
    redis::RespValue name;
    const auto parsed = redis::UnpackOne(std::string_view(stream_).substr(line_end + 2),
                                         &name, 1, limits_);
    if (parsed.status == redis::RespStatus::kNeedMore) return std::nullopt;
    if (parsed.status != redis::RespStatus::kOk ||
        name.type != redis::RespType::kBulkString ||
        !accept(name.text, static_cast<std::size_t>(count))) {
      protocol_error_ = true;
      protocol_error_text_ = "command array length or name rejected";
      return std::nullopt;
    }
  }
  // Each bulk string requires at least "$0\r\n\r\n". Do not allocate for a
  // large declared array before its contents arrive.
  if (static_cast<std::uint64_t>(count) > (stream_.size() - line_end - 2) / 6) {
    return std::nullopt;
  }
  scratch_.resize(static_cast<std::size_t>(count) + 1);

  redis::RespResult result =
      redis::UnpackOne(stream_, scratch_.data(), scratch_.size(), limits_);
  if (result.status == redis::RespStatus::kNeedMore) {
    return std::nullopt;
  }

  if (result.status != redis::RespStatus::kOk || result.value == nullptr) {
    protocol_error_ = true;
    protocol_error_text_ = result.error == nullptr || result.error[0] == '\0'
                               ? "RESP protocol error"
                               : result.error;
    return std::nullopt;
  }

  CommandArgs command;
  if (!ConvertCommand(*result.value, &command)) {
    protocol_error_ = true;
    protocol_error_text_ = "expected RESP array of bulk strings";
    return std::nullopt;
  }

  stream_.erase(0, result.consumed);
  return command;
}

bool RespCodec::HasProtocolError() const { return protocol_error_; }

const std::string& RespCodec::ProtocolError() const {
  return protocol_error_text_;
}

std::size_t RespCodec::BufferedBytes() const { return stream_.size(); }

bool RespCodec::ConvertCommand(const redis::RespValue& value,
                               CommandArgs* out) {
  if (value.type != redis::RespType::kArray) {
    return false;
  }

  out->args.clear();
  out->args.reserve(value.element_count);
  for (std::size_t i = 0; i < value.element_count; ++i) {
    const redis::RespValue& element = value.elements[i];
    if (element.type != redis::RespType::kBulkString) {
      out->args.clear();
      return false;
    }
    out->args.emplace_back(element.text.data(), element.text.size());
  }
  return true;
}

void PackResponse(const Response& response, std::string* out) {
  switch (response.type) {
    case ResponseType::kSimpleString:
      redis::PackSimpleString(response.text, out);
      break;
    case ResponseType::kError:
      redis::PackError(response.text, out);
      break;
    case ResponseType::kInteger:
      redis::PackInteger(response.integer, out);
      break;
    case ResponseType::kBulkString:
      redis::PackBulkString(response.text, out);
      break;
    case ResponseType::kNullBulkString:
      redis::PackNullBulkString(out);
      break;
    case ResponseType::kArray:
      PackResponseElements(response.elements, out);
      break;
  }
}

}  // namespace protocol
