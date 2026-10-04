#pragma once

#include <optional>
#include <string>

#include "cache/redis_object.h"

namespace repl {

std::string EncodeSnapshotPayload(const cache::ObjectMap& map);
std::optional<cache::ObjectMap> DecodeSnapshotPayload(
    std::string_view payload);

}  // namespace repl
