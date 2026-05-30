#include "repl/snapshot_codec.h"

#include <array>
#include <charconv>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/parse_utils.h"
#include "redis/resp.h"

namespace repl {
namespace {

constexpr std::string_view kMagic = "CACHE.SNAPSHOT.V1";
constexpr std::size_t kMaxSnapshotElements = 1'000'000;

std::string U64(std::uint64_t value) { return std::to_string(value); }

std::optional<std::uint64_t> ParseU64(std::string_view text) {
  std::uint64_t value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::string FormatScore(double score) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << score;
  std::string text = out.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

void PackBulk(std::string_view value, std::string* out) {
  redis::PackBulkString(value, out);
}

bool IsBulk(const redis::RespValue& value) {
  return value.type == redis::RespType::kBulkString;
}

}  // namespace

std::string EncodeSnapshotPayload(const cache::ObjectMap& map,
                                  std::uint64_t now_us) {
  std::vector<std::pair<cache::PackedString, cache::RedisObject>> live;
  map.ForEach([&](const cache::PackedString& key,
                  const cache::RedisObject& object) {
    if (!object.IsExpired(now_us)) {
      live.push_back({key, object});
    }
  });

  std::size_t elements = 2;
  for (const auto& item : live) {
    const cache::RedisObject& object = item.second;
    elements += 4;
    if (object.Type() == cache::RedisObjectType::kString) {
      elements += 1;
    } else if (object.Type() == cache::RedisObjectType::kHash) {
      elements += object.Hash()->Size() * 2;
    } else if (object.Type() == cache::RedisObjectType::kSet) {
      elements += object.Set()->Size();
    } else {
      elements += object.ZSet()->Size() * 2;
    }
  }

  std::string out;
  redis::PackArrayHeader(elements, &out);
  PackBulk(kMagic, &out);
  PackBulk(std::to_string(live.size()), &out);
  for (const auto& item : live) {
    const std::string key = item.first.ToString();
    const cache::RedisObject& object = item.second;
    PackBulk(key, &out);
    PackBulk(U64(object.DeadlineUs()), &out);
    if (object.Type() == cache::RedisObjectType::kString) {
      PackBulk("string", &out);
      PackBulk("1", &out);
      PackBulk(object.StringValue()->ToString(), &out);
    } else if (object.Type() == cache::RedisObjectType::kHash) {
      PackBulk("hash", &out);
      PackBulk(std::to_string(object.Hash()->Size() * 2), &out);
      object.Hash()->ForEach([&](const cache::PackedString& field,
                                 const cache::PackedString& value) {
        PackBulk(field.ToString(), &out);
        PackBulk(value.ToString(), &out);
      });
    } else if (object.Type() == cache::RedisObjectType::kSet) {
      PackBulk("set", &out);
      PackBulk(std::to_string(object.Set()->Size()), &out);
      object.Set()->ForEach([&](const cache::PackedString& member) {
        PackBulk(member.ToString(), &out);
      });
    } else {
      PackBulk("zset", &out);
      PackBulk(std::to_string(object.ZSet()->Size() * 2), &out);
      object.ZSet()->ForEach([&](const cache::PackedString& member,
                                 double score) {
        PackBulk(member.ToString(), &out);
        PackBulk(FormatScore(score), &out);
      });
    }
  }
  return out;
}

std::optional<cache::ObjectMap> DecodeSnapshotPayload(
    std::string_view payload) {
  std::vector<redis::RespValue> scratch(kMaxSnapshotElements);
  redis::RespLimits limits;
  limits.max_array_elements = kMaxSnapshotElements;
  limits.max_bulk_bytes = payload.size();
  redis::RespResult result =
      redis::UnpackOne(payload, scratch.data(), scratch.size(), limits);
  if (result.status != redis::RespStatus::kOk || result.consumed != payload.size() ||
      result.value == nullptr || result.value->type != redis::RespType::kArray ||
      result.value->element_count < 2) {
    return std::nullopt;
  }
  const redis::RespValue& root = *result.value;
  for (std::size_t i = 0; i < root.element_count; ++i) {
    if (!IsBulk(root.elements[i])) {
      return std::nullopt;
    }
  }
  if (root.elements[0].text != kMagic) {
    return std::nullopt;
  }
  auto object_count = ParseU64(root.elements[1].text);
  if (!object_count.has_value()) {
    return std::nullopt;
  }

  cache::ObjectMap map;
  std::size_t index = 2;
  for (std::uint64_t object_index = 0; object_index < *object_count;
       ++object_index) {
    if (index + 4 > root.element_count) {
      return std::nullopt;
    }
    const std::string key(root.elements[index++].text);
    auto deadline = ParseU64(root.elements[index++].text);
    const std::string_view type = root.elements[index++].text;
    auto field_count = ParseU64(root.elements[index++].text);
    if (!deadline.has_value() || !field_count.has_value() ||
        *field_count > root.element_count - index) {
      return std::nullopt;
    }

    cache::RedisObject object;
    if (type == "string") {
      if (*field_count != 1) return std::nullopt;
      object = cache::RedisObject::MakeString(root.elements[index++].text);
    } else if (type == "hash") {
      if ((*field_count % 2) != 0) return std::nullopt;
      cache::HashValue hash;
      for (std::uint64_t i = 0; i < *field_count; i += 2) {
        cache::PackedString field(root.elements[index++].text);
        cache::PackedString value(root.elements[index++].text);
        hash = hash.Set(field, value);
      }
      object = cache::RedisObject::MakeHash(hash);
    } else if (type == "set") {
      cache::SetValue set;
      for (std::uint64_t i = 0; i < *field_count; ++i) {
        set = set.Add(cache::PackedString(root.elements[index++].text));
      }
      object = cache::RedisObject::MakeSet(set);
    } else if (type == "zset") {
      if ((*field_count % 2) != 0) return std::nullopt;
      cache::ZSetValue zset;
      for (std::uint64_t i = 0; i < *field_count; i += 2) {
        cache::PackedString member(root.elements[index++].text);
        double score = 0.0;
        if (!common::ParseFiniteDouble(root.elements[index++].text, &score)) {
          return std::nullopt;
        }
        zset = zset.Set(member, score);
      }
      object = cache::RedisObject::MakeZSet(zset);
    } else {
      return std::nullopt;
    }
    if (*deadline != 0) {
      object = object.WithDeadline(*deadline);
    }
    map = map.Set(cache::PackedString(key), object);
  }
  if (index != root.element_count) {
    return std::nullopt;
  }
  return map;
}

}  // namespace repl
