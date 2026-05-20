#include "test_harness.h"

#include "cache/cache_engine.h"
#include "command/command_dispatcher.h"
#include "protocol/resp_codec.h"

CACHE_TEST(RespPipelineExecutesThroughDispatcher) {
  cache::CacheEngine engine;
  command::CommandDispatcher dispatcher;
  protocol::RespCodec codec;
  codec.AppendBytes(
      "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n");

  std::string out;
  while (auto command = codec.NextCommand()) {
    protocol::PackResponse(dispatcher.Execute(command->args, engine, 1000),
                           &out);
  }

  test::RequireEqual(out, "+OK\r\n$1\r\n1\r\n", "pipeline response");
}
