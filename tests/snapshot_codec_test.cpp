#include "test_harness.h"

#include <string>

#include "cache/redis_object.h"
#include "common/parse_utils.h"
#include "repl/snapshot_codec.h"

namespace {

cache::ObjectMap MapWithValues() {
  cache::ObjectMap map;
  map = map.Set(cache::PackedString("str"),
                cache::RedisObject::MakeString("value").WithDeadline(5000));

  cache::HashValue hash;
  hash = hash.Set(cache::PackedString("field"), cache::PackedString("hvalue"));
  map = map.Set(cache::PackedString("hash"), cache::RedisObject::MakeHash(hash));

  cache::SetValue set;
  set = set.Add(cache::PackedString("member"));
  map = map.Set(cache::PackedString("set"), cache::RedisObject::MakeSet(set));

  cache::ZSetValue zset;
  zset = zset.Set(cache::PackedString("zmember"), 1.5);
  map = map.Set(cache::PackedString("zset"), cache::RedisObject::MakeZSet(zset));
  return map;
}

}  // namespace

CACHE_TEST(SnapshotCodecRoundTripsAllObjectTypes) {
  const std::uint64_t now_us = 1000;
  std::string payload = repl::EncodeSnapshotPayload(MapWithValues(), now_us);

  auto decoded = repl::DecodeSnapshotPayload(payload);
  test::Require(decoded.has_value(), "snapshot payload decodes");

  const cache::RedisObject* str =
      decoded->Find(cache::PackedString("str"));
  test::Require(str != nullptr, "string key decoded");
  test::Require(str->DeadlineUs() == 5000, "deadline preserved");
  test::RequireEqual(str->StringValue()->ToString(), "value", "string value");

  const cache::RedisObject* hash =
      decoded->Find(cache::PackedString("hash"));
  test::Require(hash != nullptr, "hash key decoded");
  test::RequireEqual(hash->Hash()->Find(cache::PackedString("field"))->ToString(),
                     "hvalue", "hash field value");

  const cache::RedisObject* set = decoded->Find(cache::PackedString("set"));
  test::Require(set != nullptr, "set key decoded");
  test::Require(set->Set()->Contains(cache::PackedString("member")),
                "set member decoded");

  const cache::RedisObject* zset =
      decoded->Find(cache::PackedString("zset"));
  test::Require(zset != nullptr, "zset key decoded");
  const double* score = zset->ZSet()->Find(cache::PackedString("zmember"));
  test::Require(score != nullptr && *score == 1.5, "zset score decoded");
}

CACHE_TEST(SnapshotCodecSkipsExpiredObjects) {
  cache::ObjectMap map;
  map = map.Set(cache::PackedString("expired"),
                cache::RedisObject::MakeString("old").WithDeadline(1000));
  map = map.Set(cache::PackedString("live"),
                cache::RedisObject::MakeString("new").WithDeadline(3000));

  std::string payload = repl::EncodeSnapshotPayload(map, 2000);
  auto decoded = repl::DecodeSnapshotPayload(payload);
  test::Require(decoded.has_value(), "snapshot payload decodes");
  test::Require(decoded->Find(cache::PackedString("expired")) == nullptr,
                "expired object skipped");
  test::Require(decoded->Find(cache::PackedString("live")) != nullptr,
                "live object retained");
}

CACHE_TEST(SnapshotCodecRejectsMalformedPayload) {
  auto decoded = repl::DecodeSnapshotPayload("not-resp");
  test::Require(!decoded.has_value(), "malformed payload rejected");
}
