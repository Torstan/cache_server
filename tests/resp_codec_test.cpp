#include "test_harness.h"

#include "protocol/resp_codec.h"

using protocol::RespCodec;
using protocol::Response;

CACHE_TEST(RespCodecParsesPipelineCommands) {
  RespCodec codec;
  codec.AppendBytes("*2\r\n$3\r\nGET\r\n$1\r\na\r\n*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\nc\r\n");

  auto first = codec.NextCommand();
  test::Require(first.has_value(), "first command is available");
  test::RequireEqual(first->args[0], "GET", "first command name");
  test::RequireEqual(first->args[1], "a", "first key");

  auto second = codec.NextCommand();
  test::Require(second.has_value(), "second command is available");
  test::RequireEqual(second->args[0], "SET", "second command name");
  test::RequireEqual(second->args[2], "c", "second value");
}

CACHE_TEST(RespCodecPacksResponses) {
  std::string out;
  protocol::PackResponse(Response::SimpleString("OK"), &out);
  protocol::PackResponse(Response::Integer(2), &out);
  protocol::PackResponse(Response::BulkString("abc"), &out);
  protocol::PackResponse(Response::NullBulk(), &out);
  test::RequireEqual(out, "+OK\r\n:2\r\n$3\r\nabc\r\n$-1\r\n", "packed RESP");
}

CACHE_TEST(RespCodecHonorsConfiguredArrayLimit) {
  RespCodec codec(1024 * 1024, 1024, 300);
  std::string command = "*300\r\n";
  for (int i = 0; i < 300; ++i) {
    command += "$1\r\na\r\n";
  }

  test::Require(codec.AppendBytes(command), "large command appends");
  auto parsed = codec.NextCommand();
  test::Require(parsed.has_value(), "large command parses");
  test::Require(parsed->args.size() == 300, "configured array limit is honored");
}

CACHE_TEST(RespCodecDefaultLimitsAcceptRedisUnitBigPayload) {
  RespCodec codec;
  std::string payload(4 * 1000 * 1000, 'x');
  std::string command = "*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$";
  command += std::to_string(payload.size());
  command += "\r\n";
  command += payload;
  command += "\r\n";

  test::Require(codec.AppendBytes(command), "big payload appends");
  auto parsed = codec.NextCommand();
  test::Require(parsed.has_value(), "big payload command parses");
  test::RequireEqual(parsed->args[0], "SET", "big payload command name");
  test::RequireEqual(parsed->args[1], "big", "big payload key");
  test::Require(parsed->args[2].size() == payload.size(),
                "big payload value length is preserved");
}

CACHE_TEST(RespCodecDefaultLimitsAcceptRedisUnitWideHashCommand) {
  RespCodec codec;
  std::string command = "*258\r\n$4\r\nHSET\r\n$7\r\nbighash\r\n";
  for (int i = 0; i < 128; ++i) {
    const std::string field = "field:" + std::to_string(i);
    const std::string value = "value:" + std::to_string(i);
    command += "$" + std::to_string(field.size()) + "\r\n" + field + "\r\n";
    command += "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
  }

  test::Require(codec.AppendBytes(command), "wide hash command appends");
  auto parsed = codec.NextCommand();
  test::Require(parsed.has_value(), "wide hash command parses");
  test::RequireEqual(parsed->args[0], "HSET", "wide hash command name");
  test::Require(parsed->args.size() == 258,
                "wide hash command preserves every argument");
}

CACHE_TEST(RespCodecParsesCommandAtArrayLimit) {
  RespCodec codec(1024 * 1024, 1024, 1024);
  std::string command = "*1024\r\n";
  for (int i = 0; i < 1024; ++i) {
    command += "$1\r\na\r\n";
  }

  test::Require(codec.AppendBytes(command), "limit-sized command appends");
  auto parsed = codec.NextCommand();
  test::Require(parsed.has_value(), "limit-sized command parses");
  test::Require(parsed->args.size() == 1024,
                "array at configured limit preserves every argument");
}
