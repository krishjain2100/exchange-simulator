#include "NetworkServer.h"
#include "OrderParser.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/types.h>
#endif

namespace {

constexpr int kListenPort = 8080;
constexpr int kListenBacklog = 100;
constexpr int kMaxEvents = 64;
constexpr int kRecvChunk = 4096;
constexpr int kEpollWaitMs = 100;
constexpr size_t kCompactThreshold = 4096;

struct ClientSession {
  std::vector<char> buf;
  size_t offset = 0;

  void Append(const char *data, size_t len) {
    if (offset >= kCompactThreshold && offset > buf.size() / 2) {
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
      offset = 0;
    }
    buf.insert(buf.end(), data, data + len);
  }
};

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool HandleParsedMessage(const ParsedMessage &msg, RunContext &ctx,
                         bool &stop_session) {
  if (msg.kind == ParsedMessage::Kind::PoisonPill) {
    std::cout << "\n[Wrapper] Poison Pill Received. Commencing Time Based "
                 "Shutdown...\n";
    ctx.SignalIngressComplete();
    stop_session = true;
    return true;
  }

  if (msg.kind == ParsedMessage::Kind::Order) {
    ctx.EnqueueOrder(msg.order);
  }

  return false;
}

void ProcessClientBuffer(ClientSession &session, RunContext &ctx,
                         bool &stop_session) {
  constexpr size_t kFrameSize = sizeof(Order);

  while (!stop_session) {
    const size_t avail = session.buf.size() - session.offset;
    if (avail < kFrameSize)
      break;

    const char *base = session.buf.data() + session.offset;
    const ParsedMessage msg = ParseOrderFrame(base, kFrameSize);
    if (msg.kind != ParsedMessage::Kind::Invalid)
      HandleParsedMessage(msg, ctx, stop_session);

    session.offset += kFrameSize;
  }

  if (session.offset >= session.buf.size()) {
    session.buf.clear();
    session.offset = 0;
  } else if (session.offset >= kCompactThreshold) {
    session.buf.erase(session.buf.begin(),
                      session.buf.begin() +
                          static_cast<std::ptrdiff_t>(session.offset));
    session.offset = 0;
  }
}

ssize_t DrainClientSocket(int fd, ClientSession &session, RunContext &ctx,
                          bool &stop_session) {
  char chunk[kRecvChunk];

  while (!stop_session) {
    const ssize_t bytes =
        recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
    if (bytes > 0) {
      session.Append(chunk, static_cast<size_t>(bytes));
      ProcessClientBuffer(session, ctx, stop_session);
      continue;
    }

    if (bytes == 0) return 0;

    if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
    return -1;
  }

  return 1;
}

void CloseClient(int fd, std::unordered_map<int, ClientSession> &clients,
                 RunContext &ctx
#if defined(__linux__)
                 ,
                 int epfd
#elif defined(__APPLE__)
                 ,
                 int kq
#endif
) {
  clients.erase(fd);
  ctx.OnClientDisconnected();
  close(fd);
#if defined(__linux__)
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
#elif defined(__APPLE__)
  struct kevent ev{};
  EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  kevent(kq, &ev, 1, nullptr, 0, nullptr);
#endif
}

void AcceptPendingClients(
    int server_fd, RunContext &ctx,
    std::unordered_map<int, ClientSession> &clients
#if defined(__linux__)
    ,
    int epfd
#elif defined(__APPLE__)
    ,
    int kq
#endif
) {
  while (!ctx.ShouldStopAccepting()) {
    const int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      perror("[Wrapper ERROR] Accept failed");
      break;
    }

    if (!SetNonBlocking(client_fd)) {
      close(client_fd);
      continue;
    }

#if defined(__linux__)
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = client_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
      close(client_fd);
      continue;
    }
#elif defined(__APPLE__)
    struct kevent ev{};
    EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) {
      close(client_fd);
      continue;
    }
#endif

    clients.emplace(client_fd, ClientSession{});
    clients[client_fd].buf.reserve(kRecvChunk);
    ctx.OnClientConnected();
  }
}

#if defined(__linux__)

void RunIngressLoop(int server_fd, int epfd, RunContext &ctx) {
  std::unordered_map<int, ClientSession> clients;

  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = server_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

  epoll_event events[kMaxEvents];

  while (!ctx.ShouldStopAccepting()) {
    if (ctx.IsShutdown())
      break;
    const int ready =
        epoll_wait(epfd, events, kMaxEvents, kEpollWaitMs);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("[Wrapper ERROR] epoll_wait failed");
      break;
    }

    for (int i = 0; i < ready; ++i) {
      const int fd = events[i].data.fd;

      if (fd == server_fd) {
        AcceptPendingClients(server_fd, ctx, clients, epfd);
        continue;
      }

      auto client_it = clients.find(fd);
      if (client_it == clients.end()) continue;

      bool stop_session = false;
      ssize_t status =
          DrainClientSocket(fd, client_it->second, ctx, stop_session);

      if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        while (!stop_session && status > 0) {
          status = DrainClientSocket(fd, client_it->second, ctx, stop_session);
        }
        CloseClient(fd, clients, ctx, epfd);
        continue;
      }

      if (stop_session || status <= 0) {
        CloseClient(fd, clients, ctx, epfd);
      }
    }
  }

  for (auto it = clients.begin(); it != clients.end();) {
    CloseClient(it->first, clients, ctx, epfd);
    it = clients.begin();
  }
}

#elif defined(__APPLE__)

void RunIngressLoop(int server_fd, int kq, RunContext &ctx) {
  std::unordered_map<int, ClientSession> clients;

  struct kevent ev{};
  EV_SET(&ev, server_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  kevent(kq, &ev, 1, nullptr, 0, nullptr);

  struct kevent events[kMaxEvents];
  timespec timeout{0, kEpollWaitMs * 1000000L};

  while (!ctx.ShouldStopAccepting()) {
    if (ctx.IsShutdown())
      break;
    const int ready =
        kevent(kq, nullptr, 0, events, kMaxEvents, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("[Wrapper ERROR] kevent failed");
      break;
    }

    for (int i = 0; i < ready; ++i) {
      const int fd = static_cast<int>(events[i].ident);

      if (fd == server_fd) {
        AcceptPendingClients(server_fd, ctx, clients, kq);
        continue;
      }

      auto client_it = clients.find(fd);
      if (client_it == clients.end()) continue;

      bool stop_session = false;
      ssize_t status =
          DrainClientSocket(fd, client_it->second, ctx, stop_session);

      if (events[i].flags & EV_EOF) {
        while (!stop_session && status > 0) {
          status = DrainClientSocket(fd, client_it->second, ctx, stop_session);
        }
        CloseClient(fd, clients, ctx, kq);
        continue;
      }

      if (stop_session || status <= 0) {
        CloseClient(fd, clients, ctx, kq);
      }
    }
  }

  while (!clients.empty()) {
    CloseClient(clients.begin()->first, clients, ctx, kq);
  }
}

#else
#error "NetworkServer requires Linux (epoll) or macOS (kqueue)"
#endif

} // namespace

void RunNetworkServer(RunContext &ctx) {
  const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  ctx.SetServerFd(server_fd);

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(kListenPort);

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&address),
           sizeof(address)) < 0) {
    perror("[Wrapper ERROR] Bind failed");
    _exit(1);
  }

  listen(server_fd, kListenBacklog);
  SetNonBlocking(server_fd);

  std::cout << "[Wrapper] Listening on Port " << kListenPort
            << ". Awaiting Orders..." << std::endl;

#if defined(__linux__)
  const int epfd = epoll_create1(0);
  if (epfd < 0) {
    perror("[Wrapper ERROR] epoll_create1 failed");
    _exit(1);
  }
  RunIngressLoop(server_fd, epfd, ctx);
  close(epfd);
#elif defined(__APPLE__)
  const int kq = kqueue();
  if (kq < 0) {
    perror("[Wrapper ERROR] kqueue failed");
    _exit(1);
  }
  RunIngressLoop(server_fd, kq, ctx);
  close(kq);
#endif

  close(server_fd);
}
