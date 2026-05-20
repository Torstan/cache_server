#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "immutable_container/imt_map.h"
#include "immutable_container/imt_set.h"
#include "immutable_container/packed_string.h"
#include "immutable_container/ref_count_policy.h"

namespace cache {

using immutable_container::AtomicRefCount;
using immutable_container::ImtMap;
using immutable_container::ImtSet;
using immutable_container::PackedString;

enum class Status { kOk, kNotFound, kWrongType, kInvalidArgument };

template <typename T>
struct ReadResult {
  Status status = Status::kNotFound;
  T value{};
};

struct WriteResult {
  Status status = Status::kOk;
  bool changed = false;
  bool created = false;
  std::uint64_t seq = 0;
};

using HashValue =
    ImtMap<PackedString, PackedString, std::less<PackedString>, AtomicRefCount>;
using SetValue = ImtSet<PackedString, std::less<PackedString>, AtomicRefCount>;
using ZSetValue =
    ImtMap<PackedString, double, std::less<PackedString>, AtomicRefCount>;

enum class RedisObjectType { kString, kHash, kSet, kZSet };

class RedisObject {
 public:
  static RedisObject MakeString(std::string_view value);
  static RedisObject MakeHash(HashValue value);
  static RedisObject MakeSet(SetValue value);
  static RedisObject MakeZSet(ZSetValue value);

  RedisObjectType Type() const;
  bool IsExpired(std::uint64_t now_us) const;
  std::uint64_t DeadlineUs() const;
  RedisObject WithDeadline(std::uint64_t deadline_us) const;
  RedisObject ClearDeadline() const;

  const PackedString* StringValue() const;
  const HashValue* Hash() const;
  const SetValue* Set() const;
  const ZSetValue* ZSet() const;

 private:
  RedisObjectType type_ = RedisObjectType::kString;
  std::uint64_t deadline_us_ = 0;
  std::variant<PackedString, HashValue, SetValue, ZSetValue> value_;
};

using ObjectMap =
    ImtMap<PackedString, RedisObject, std::less<PackedString>, AtomicRefCount>;

}  // namespace cache
