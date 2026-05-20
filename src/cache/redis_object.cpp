#include "cache/redis_object.h"

#include <utility>

namespace cache {

RedisObject RedisObject::MakeString(std::string_view value) {
  RedisObject object;
  object.type_ = RedisObjectType::kString;
  object.deadline_us_ = 0;
  object.value_ = PackedString(value);
  return object;
}

RedisObject RedisObject::MakeHash(HashValue value) {
  RedisObject object;
  object.type_ = RedisObjectType::kHash;
  object.deadline_us_ = 0;
  object.value_ = std::move(value);
  return object;
}

RedisObject RedisObject::MakeSet(SetValue value) {
  RedisObject object;
  object.type_ = RedisObjectType::kSet;
  object.deadline_us_ = 0;
  object.value_ = std::move(value);
  return object;
}

RedisObject RedisObject::MakeZSet(ZSetValue value) {
  RedisObject object;
  object.type_ = RedisObjectType::kZSet;
  object.deadline_us_ = 0;
  object.value_ = std::move(value);
  return object;
}

RedisObjectType RedisObject::Type() const { return type_; }

bool RedisObject::IsExpired(std::uint64_t now_us) const {
  return deadline_us_ != 0 && now_us >= deadline_us_;
}

std::uint64_t RedisObject::DeadlineUs() const { return deadline_us_; }

RedisObject RedisObject::WithDeadline(std::uint64_t deadline_us) const {
  RedisObject object = *this;
  object.deadline_us_ = deadline_us;
  return object;
}

RedisObject RedisObject::ClearDeadline() const {
  RedisObject object = *this;
  object.deadline_us_ = 0;
  return object;
}

const PackedString* RedisObject::StringValue() const {
  return std::get_if<PackedString>(&value_);
}

const HashValue* RedisObject::Hash() const {
  return std::get_if<HashValue>(&value_);
}

const SetValue* RedisObject::Set() const {
  return std::get_if<SetValue>(&value_);
}

const ZSetValue* RedisObject::ZSet() const {
  return std::get_if<ZSetValue>(&value_);
}

}  // namespace cache
