#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cache/binlog.h"
#include "cache/cache_engine.h"
#include "cache/redis_object.h"

namespace {

cache::BinlogRecord MakeRecord(cache::BinlogOp op,
                               std::vector<std::string> args) {
  cache::BinlogRecord record;
  record.op = op;
  record.args = std::move(args);
  return record;
}

}  // namespace

CACHE_TEST(GenericSetAndGet) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;

  auto r = engine.Set("k", cache::RedisObject::MakeString("v"),
                      MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}),
                      now);
  test::Require(r.status == cache::Status::kOk, "set ok");
  test::Require(r.created, "created");

  auto obj = engine.Get("k", now);
  test::Require(obj.has_value(), "get found");
  const cache::PackedString* value = obj->StringValue();
  test::Require(value != nullptr, "string pointer exists");
  test::RequireEqual(value->ToString(), std::string("v"), "value");
}

CACHE_TEST(GenericGetMissing) {
  cache::CacheEngine engine;
  auto obj = engine.Get("missing", 1'000'000);
  test::Require(!obj.has_value(), "get missing returns nullopt");
}

CACHE_TEST(GenericUpdateCreates) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;

  auto r = engine.Update("k", [](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    test::Require(!existing.has_value(), "no existing");
    return cache::RedisObject::MakeString("new");
  }, MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "new"}), now);

  test::Require(r.created, "created");
  auto obj = engine.Get("k", now);
  const cache::PackedString* value = obj->StringValue();
  test::Require(value != nullptr, "string pointer exists");
  test::RequireEqual(value->ToString(), std::string("new"), "value");
}

CACHE_TEST(GenericUpdateModifies) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;
  engine.Set("k", cache::RedisObject::MakeString("old"),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "old"}), now);

  auto r = engine.Update("k", [](std::optional<cache::RedisObject> existing)
      -> std::optional<cache::RedisObject> {
    test::Require(existing.has_value(), "has existing");
    return cache::RedisObject::MakeString("new");
  }, MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "new"}), now);

  test::Require(!r.created, "not created");
  auto obj = engine.Get("k", now);
  const cache::PackedString* value = obj->StringValue();
  test::Require(value != nullptr, "string pointer exists");
  test::RequireEqual(value->ToString(), std::string("new"), "value");
}

CACHE_TEST(GenericUpdateCancels) {
  cache::CacheEngine engine;
  const std::uint64_t now = 1'000'000;
  engine.Set("k", cache::RedisObject::MakeString("v"),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}), now);
  const std::size_t before_logs =
      engine.SlotForKey("k").CopyLogsAfter(0, 10).size();

  auto r = engine.Update("k", [](std::optional<cache::RedisObject>)
      -> std::optional<cache::RedisObject> {
    return std::nullopt;
  }, MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "ignored"}), now);

  test::Require(r.status == cache::Status::kOk, "cancelled cleanly");
  test::Require(!r.changed, "not changed");
  const std::size_t after_logs =
      engine.SlotForKey("k").CopyLogsAfter(0, 10).size();
  test::Require(after_logs == before_logs, "cancel does not append binlog");
}

CACHE_TEST(GenericMutateDeletesExistingKey) {
  cache::CacheEngine engine;
  const std::uint64_t now = 10;
  engine.Set("k", cache::RedisObject::MakeString("v"),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}), now);

  auto result = engine.Mutate(
      "k",
      [](std::optional<cache::RedisObject>) {
        return cache::MutationResult{true, std::nullopt};
      },
      MakeRecord(cache::BinlogOp::kDel, {"CUSTOMDEL", "k"}), now);

  test::Require(result.changed, "Mutate delete changes key");
  test::Require(!engine.Get("k", now).has_value(), "Mutate deletes key");
}

CACHE_TEST(GenericMutateNoOpDoesNotAppendLog) {
  cache::CacheEngine engine;
  const std::uint64_t now = 10;

  auto result = engine.Mutate(
      "missing",
      [](std::optional<cache::RedisObject>) {
        return cache::MutationResult{false, std::nullopt};
      },
      MakeRecord(cache::BinlogOp::kSet, {"NOOP", "missing"}), now);

  test::Require(!result.changed, "Mutate no-op reports unchanged");
  test::Require(engine.SlotForKey("missing").CopyLogsAfter(0, 10).empty(),
                "Mutate no-op does not append binlog");
}

CACHE_TEST(GenericMutateDeleteExpiredKeyIsNoOp) {
  cache::CacheEngine engine;
  const std::uint64_t now = 10;
  engine.Set("k", cache::RedisObject::MakeString("v").WithDeadline(now + 1),
             MakeRecord(cache::BinlogOp::kSet, {"SET", "k", "v"}), now);
  const std::size_t before_logs =
      engine.SlotForKey("k").CopyLogsAfter(0, 10).size();

  auto result = engine.Mutate(
      "k",
      [](std::optional<cache::RedisObject> existing) {
        test::Require(!existing.has_value(),
                      "expired object is not passed to mutator");
        return cache::MutationResult{true, std::nullopt};
      },
      MakeRecord(cache::BinlogOp::kDel, {"CUSTOMDEL", "k"}), now + 2);

  test::Require(!result.changed, "Mutate delete expired key is unchanged");
  const std::size_t after_logs =
      engine.SlotForKey("k").CopyLogsAfter(0, 10).size();
  test::Require(after_logs == before_logs,
                "Mutate delete expired key does not append binlog");
}
