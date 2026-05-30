#include "net/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <stack>
#include <thread>
#include <utility>
#include <vector>

#include "co_epoll.h"
#include "co_routine.h"
#include "co_timeout.h"
#include "common/hash.h"
#include "common/parse_utils.h"
#include "common/time.h"
#include "protocol/resp_codec.h"
#include "protocol/response.h"
#include "repl/slave_replicator.h"
#include "thread_worker.h"

namespace net {
namespace {

using co::Coroutine;
using co::ThreadWorker;
using co::TimeoutItem;
using co::TimeoutItemLink;
using co::co_accept;
using co::co_create;
using co::co_enable_hook_sys;
using co::co_get_curr_thread_env;
using co::co_poll;
using co::co_resume;
using co::co_yield_ct;

constexpr std::size_t kPendingFdLimit = 4096;

[[maybe_unused]] static bool IsCacheReplCommand(
    const std::vector<std::string>& args) {
  return !args.empty() && common::ToUpperAscii(args[0]) == "CACHE.REPL";
}

struct Task {
  Coroutine* coroutine = nullptr;
  int fd = -1;
  cache::CacheEngine* engine = nullptr;
  const command::CommandDispatcher* dispatcher = nullptr;
  const ServerConfig* config = nullptr;
  repl::SlaveReplicator* slave_replicator = nullptr;
  TimeoutItem io_event;
  epoll_event event;
};

class Worker {
 public:
  Worker(int worker_id, const ServerConfig* config, cache::CacheEngine* engine,
         const command::CommandDispatcher* dispatcher,
         repl::SlaveReplicator* slave_replicator)
      : worker_id_(worker_id),
        config_(config),
        engine_(engine),
        dispatcher_(dispatcher),
        slave_replicator_(slave_replicator) {}

  void DispatchFd(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_fds_.size() >= kPendingFdLimit) {
      close(fd);
      return;
    }
    pending_fds_.push_back(fd);
    has_pending_fds_.store(true, std::memory_order_relaxed);
  }

  void Run(int coroutine_count) {
    co_enable_hook_sys();
    const int pool_size = std::max(1, coroutine_count);
    for (int i = 0; i < pool_size; ++i) {
      Task* task = new Task;
      task->engine = engine_;
      task->dispatcher = dispatcher_;
      task->config = config_;
      task->slave_replicator = slave_replicator_;
      task->coroutine = co_create([task]() { ConnectionRoutine(task); });
      co_resume(task->coroutine);
    }

    Coroutine* reap_coroutine = co_create([this]() { ReapFdsRoutine(this); });
    co_resume(reap_coroutine);

    ThreadWorker worker(worker_id_);
    worker.run_loop();
  }

 private:
  static void OnFdReady(TimeoutItem* item) {
    Task* task = static_cast<Task*>(item->arg);
    co_resume(task->coroutine);
  }

  static bool RegisterFdEvent(Task* task) {
    TimeoutItemLink::remove(&task->io_event);
    task->io_event.arg = task;
    task->io_event.prepare_func = nullptr;
    task->io_event.process_func = OnFdReady;
    task->io_event.timeout = false;
    task->event = {};
    task->event.data.ptr = &task->io_event;
    task->event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    return co_get_curr_thread_env()->Epoll()->add(task->fd, &task->event) == 0;
  }

  static void CloseTask(Task* task) {
    if (task->fd < 0) {
      return;
    }
    TimeoutItemLink::remove(&task->io_event);
    co_get_curr_thread_env()->Epoll()->del(task->fd, &task->event);
    close(task->fd);
    task->fd = -1;
  }

  static bool WriteAll(int fd, const std::string& bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
      const ssize_t ret =
          write(fd, bytes.data() + written, bytes.size() - written);
      if (ret > 0) {
        written += static_cast<std::size_t>(ret);
        continue;
      }
      if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLOUT | POLLERR | POLLHUP;
        co_poll(&pfd, 1, 1000);
        continue;
      }
      return false;
    }
    return true;
  }

  static bool ProcessInput(Task* task, protocol::RespCodec* codec,
                           std::string* out) {
    while (auto command = codec->NextCommand()) {
      if (command->args.empty()) {
        continue;
      }
      if (task->config != nullptr &&
          task->config->role == ServerRole::kReplica &&
          task->dispatcher->IsWriteCommand(command->args[0])) {
        protocol::PackResponse(
            protocol::Response::Error("READONLY replica does not accept writes"),
            out);
        continue;
      }
      if (task->config != nullptr &&
          task->config->role == ServerRole::kReplica &&
          task->config->replica_reads && command->args.size() >= 2 &&
          !task->dispatcher->IsWriteCommand(command->args[0])) {
        const std::size_t slot = common::SlotForKey(command->args[1]);
        if (task->slave_replicator != nullptr &&
            !task->slave_replicator->CanReadSlot(slot)) {
          protocol::PackResponse(
              protocol::Response::Error("TRYAGAIN slot is syncing"), out);
          continue;
        }
      }
      protocol::PackResponse(
          task->dispatcher->Execute(command->args, *task->engine,
                                   common::NowMicros()),
          out);
    }
    if (!codec->HasProtocolError()) {
      return true;
    }

    protocol::PackResponse(
        protocol::Response::Error("ERR " + codec->ProtocolError()), out);
    WriteAll(task->fd, *out);
    return false;
  }

  static void ConnectionRoutine(Task* task) {
    co_enable_hook_sys();
    thread_local std::stack<Task*>* idle_tasks = nullptr;
    idle_tasks = &IdleTasks();

    char buffer[16 * 1024];
    for (;;) {
      if (task->fd < 0) {
        idle_tasks->push(task);
        co_yield_ct();
        continue;
      }

      protocol::RespCodec codec;
      for (;;) {
        const ssize_t ret = read(task->fd, buffer, sizeof(buffer));
        if (ret > 0) {
          if (!codec.AppendBytes(std::string_view(
                  buffer, static_cast<std::size_t>(ret)))) {
            std::string out;
            protocol::PackResponse(
                protocol::Response::Error("ERR " + codec.ProtocolError()),
                &out);
            WriteAll(task->fd, out);
            CloseTask(task);
            break;
          }

          std::string out;
          if (!ProcessInput(task, &codec, &out)) {
            CloseTask(task);
            break;
          }
          if (!out.empty() && !WriteAll(task->fd, out)) {
            CloseTask(task);
            break;
          }
          continue;
        }

        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          co_yield_ct();
          continue;
        }

        CloseTask(task);
        break;
      }
    }
  }

  static std::stack<Task*>& IdleTasks() {
    static thread_local std::stack<Task*> idle_tasks;
    return idle_tasks;
  }

  int ReapFds() {
    std::stack<Task*>& idle_tasks = IdleTasks();
    if (idle_tasks.empty()) {
      return 0;
    }

    if (!has_pending_fds_.load(std::memory_order_relaxed)) {
      return 0;
    }

    std::list<int> pending_fds;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_fds.swap(pending_fds_);
      has_pending_fds_.store(false, std::memory_order_relaxed);
    }

    while (!pending_fds.empty() && !idle_tasks.empty()) {
      const int fd = pending_fds.front();
      pending_fds.pop_front();

      Task* task = idle_tasks.top();
      idle_tasks.pop();
      task->fd = fd;
      if (!RegisterFdEvent(task)) {
        close(fd);
        task->fd = -1;
        idle_tasks.push(task);
        continue;
      }
      co_resume(task->coroutine);
    }

    if (!pending_fds.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      while (!pending_fds.empty() && pending_fds_.size() < kPendingFdLimit) {
        pending_fds_.push_back(pending_fds.front());
        pending_fds.pop_front();
      }
      while (!pending_fds.empty()) {
        close(pending_fds.front());
        pending_fds.pop_front();
      }
      has_pending_fds_.store(!pending_fds_.empty(),
                             std::memory_order_relaxed);
    }
    return 0;
  }

  static void ReapFdsRoutine(Worker* worker) {
    co_enable_hook_sys();
    for (;;) {
      worker->ReapFds();
      poll(nullptr, 0, 1);
    }
  }

  int worker_id_;
  const ServerConfig* config_;
  cache::CacheEngine* engine_;
  const command::CommandDispatcher* dispatcher_;
  repl::SlaveReplicator* slave_replicator_;
  std::mutex mutex_;
  std::list<int> pending_fds_;
  std::atomic<bool> has_pending_fds_{false};
};

thread_local int g_listen_fd = -1;
std::vector<std::unique_ptr<Worker>>* g_workers = nullptr;

int SetNonBlock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_NDELAY);
}

void SetAddr(const char* host, std::uint16_t port, sockaddr_in* addr) {
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  if (host == nullptr || host[0] == '\0' || strcmp(host, "0") == 0 ||
      strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
    addr->sin_addr.s_addr = htonl(INADDR_ANY);
    return;
  }
  addr->sin_addr.s_addr = inet_addr(host);
}

int CreateTcpSocket(const ServerConfig& config) {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return -1;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr;
  SetAddr(config.host.c_str(), config.port, &addr);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  if (SetNonBlock(fd) != 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 1024) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void AcceptRoutine() {
  co_enable_hook_sys();
  std::size_t next_worker = 0;
  for (;;) {
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    const int fd = co_accept(g_listen_fd, reinterpret_cast<sockaddr*>(&addr),
                             &len);
    if (fd < 0) {
      pollfd pfd = {};
      pfd.fd = g_listen_fd;
      pfd.events = POLLIN | POLLERR | POLLHUP;
      co_poll(&pfd, 1, 1000);
      continue;
    }

    if (SetNonBlock(fd) != 0) {
      close(fd);
      continue;
    }
    (*g_workers)[next_worker]->DispatchFd(fd);
    next_worker = (next_worker + 1) % g_workers->size();
  }
}

}  // namespace

Server::Server(ServerConfig config, cache::CacheEngine* engine,
               repl::SlaveReplicator* slave_replicator)
    : config_(std::move(config)),
      engine_(engine),
      slave_replicator_(slave_replicator) {}

int Server::Run() {
  if (engine_ == nullptr) {
    fprintf(stderr, "cache engine is null\n");
    return 1;
  }
  config_.worker_count = std::max(1, config_.worker_count);
  config_.coroutine_count_per_worker =
      std::max(1, config_.coroutine_count_per_worker);

  g_listen_fd = CreateTcpSocket(config_);
  if (g_listen_fd < 0) {
    fprintf(stderr, "failed to listen on %s:%u: %s\n", config_.host.c_str(),
            static_cast<unsigned>(config_.port), strerror(errno));
    return 1;
  }

  std::vector<std::unique_ptr<Worker>> workers;
  workers.reserve(static_cast<std::size_t>(config_.worker_count));
  g_workers = &workers;
  for (int i = 0; i < config_.worker_count; ++i) {
    workers.push_back(std::make_unique<Worker>(
        i, &config_, engine_, &dispatcher_, slave_replicator_));
    Worker* worker = workers.back().get();
    const int coroutine_count = config_.coroutine_count_per_worker;
    std::thread([worker, coroutine_count]() {
      worker->Run(coroutine_count);
    }).detach();
  }

  Coroutine* accept_coroutine = co_create([]() { AcceptRoutine(); });
  co_resume(accept_coroutine);

  ThreadWorker main_worker(-1);
  main_worker.run_loop();
  return 0;
}

}  // namespace net
