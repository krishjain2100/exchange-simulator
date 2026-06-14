// Replay orders across parallel TCP sessions. Paced mode uses one sender thread
// per session. Args: <host> <port> <file.bin> [--paced <ops> <duration_ms>]

#include "Order.h"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kNumSessions = 32;
using Clock = std::chrono::steady_clock;

int Connect(const char *host, uint16_t port) {
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

  int one = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  int sndbuf = 1 << 20;
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    close(sock);
    return -1;
  }

  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

bool SendFrame(int sock, const Order &order) {
  const ssize_t sent = send(sock, &order, sizeof(order), 0);
  return sent == static_cast<ssize_t>(sizeof(order));
}

bool SendPoisonPill(int sock) {
  Order poison{};
  poison.type = OrderType::POISON;
  return SendFrame(sock, poison);
}

int SessionForOrder(const Order &order) {
  return static_cast<int>(order.client_id % kNumSessions);
}

std::vector<Order> LoadBenchmarkFile(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "[Replay] Failed to open benchmark file: " << path << "\n";
    return {};
  }

  input.seekg(0, std::ios::end);
  const std::streamsize file_size = input.tellg();
  input.seekg(0, std::ios::beg);

  if (file_size <= 0 ||
      (file_size % static_cast<std::streamsize>(sizeof(Order))) != 0) {
    std::cerr << "[Replay] Invalid benchmark file size: " << file_size << "\n";
    return {};
  }

  const size_t count = static_cast<size_t>(file_size / sizeof(Order));
  std::vector<Order> orders(count);
  if (!input.read(reinterpret_cast<char *>(orders.data()), file_size)) {
    std::cerr << "[Replay] Failed to read benchmark file\n";
    return {};
  }
  return orders;
}

void RunSession(int sock, std::vector<Order> orders, int session_ops,
                Clock::time_point deadline, bool paced,
                std::atomic<bool> &failed, size_t &sent_out) {
  sent_out = 0;
  if (orders.empty())
    return;

  const auto interval =
      paced && session_ops > 0
          ? std::chrono::nanoseconds(1'000'000'000LL / session_ops)
          : std::chrono::nanoseconds(0);
  auto next_send = Clock::now();

  for (const Order &order : orders) {
    if (failed.load(std::memory_order_relaxed))
      return;
    if (paced && Clock::now() >= deadline)
      return;

    if (paced && interval.count() > 0) {
      const auto now = Clock::now();
      if (now < next_send)
        std::this_thread::sleep_until(next_send);
      next_send += interval;
    }

    if (!SendFrame(sock, order)) {
      failed.store(true, std::memory_order_relaxed);
      return;
    }
    ++sent_out;
  }
}

} // namespace

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  if (argc < 4) {
    std::cerr << "Usage: bot_replay <host> <port> <file.bin> [--paced <ops> "
                 "<duration_ms>]\n";
    return 1;
  }

  const char *host = argv[1];
  const uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
  const char *benchmark_path = argv[3];
  bool paced = false;
  int target_ops = 0;
  int duration_ms = 0;

  for (int i = 4; i < argc; ++i) {
    if (std::string(argv[i]) == "--paced" && i + 2 < argc) {
      paced = true;
      target_ops = std::stoi(argv[i + 1]);
      duration_ms = std::stoi(argv[i + 2]);
      i += 2;
    }
  }

  std::vector<Order> orders = LoadBenchmarkFile(benchmark_path);
  if (orders.empty())
    return 1;

  int socks[kNumSessions];
  for (int s = 0; s < kNumSessions; ++s) {
    socks[s] = Connect(host, port);
    if (socks[s] < 0) {
      std::cerr << "[Replay] Connection failed for session " << s << "\n";
      for (int i = 0; i < s; ++i)
        close(socks[i]);
      return 1;
    }
  }

  std::vector<Order> by_session[kNumSessions];
  for (const Order &order : orders) {
    const int session = SessionForOrder(order);
    by_session[session].push_back(order);
  }

  const int session_ops =
      paced && target_ops > 0
          ? std::max(1, (target_ops + kNumSessions - 1) / kNumSessions)
          : 0;
  const auto start = Clock::now();
  const auto deadline = start + std::chrono::milliseconds(
                                    duration_ms > 0 ? duration_ms : INT32_MAX);

  std::cout << "[Replay] " << kNumSessions << " sessions → " << host << ":"
            << port << " | replaying " << orders.size() << " orders from "
            << benchmark_path;
  if (paced)
    std::cout << " | paced " << target_ops << " ops (" << session_ops
              << "/session) / " << duration_ms << "ms";
  std::cout << "\n";

  std::atomic<bool> failed{false};
  size_t sent[kNumSessions] = {};
  std::vector<std::thread> threads;
  threads.reserve(kNumSessions);

  for (int s = 0; s < kNumSessions; ++s) {
    threads.emplace_back(RunSession, socks[s], std::move(by_session[s]),
                         session_ops, deadline, paced, std::ref(failed),
                         std::ref(sent[s]));
  }

  for (auto &t : threads) {
    if (t.joinable())
      t.join();
  }

  if (failed.load()) {
    for (int s = 0; s < kNumSessions; ++s)
      close(socks[s]);
    std::cerr << "[Replay] Send failed on one or more sessions\n";
    return 1;
  }

  if (!paced) {
    for (int s = 0; s < kNumSessions; ++s) {
      if (!SendPoisonPill(socks[s])) {
        for (int i = 0; i < kNumSessions; ++i)
          close(socks[i]);
        std::cerr << "[Replay] Failed to send poison pill on session " << s
                  << "\n";
        return 1;
      }
    }
  }

  for (int s = 0; s < kNumSessions; ++s)
    close(socks[s]);

  size_t total = 0;
  for (int s = 0; s < kNumSessions; ++s)
    total += sent[s];

  std::cout << "[Replay] Complete. Orders sent: " << total << "\n";
  return 0;
}
