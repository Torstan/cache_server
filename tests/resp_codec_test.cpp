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
