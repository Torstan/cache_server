#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/response.h"
#include "redis/resp.h"

namespace protocol {

struct CommandArgs {
  std::vector<std::string> args;
};

class RespCodec {
 public:
  explicit RespCodec(std::size_t max_stream_bytes = 4 * 1024 * 1024,
                     std::size_t max_bulk_bytes = 1024 * 1024,
                     std::size_t max_array_elements = 128);

  bool AppendBytes(std::string_view bytes);
  std::optional<CommandArgs> NextCommand();
  bool HasProtocolError() const;
  const std::string& ProtocolError() const;
  std::size_t BufferedBytes() const;

 private:
  bool ConvertCommand(const redis::RespValue& value, CommandArgs* out);

  std::string stream_;
  std::vector<redis::RespValue> scratch_;
  redis::RespLimits limits_;
  std::size_t max_stream_bytes_;
  bool protocol_error_ = false;
  std::string protocol_error_text_;
};

void PackResponse(const Response& response, std::string* out);

}  // namespace protocol
