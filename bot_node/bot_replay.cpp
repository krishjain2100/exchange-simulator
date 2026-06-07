// Phase 2 benchmark bot: replays a fixed binary order stream sequentially.
// Args: <host> <port> <path_to_benchmark_load.bin>

#include "Order.h"
#include <arpa/inet.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

int Connect(const char *host, uint16_t port) {
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

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

std::vector<Order> LoadBenchmarkFile(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "[Replay] Failed to open benchmark file: " << path << "\n";
    return {};
  }

  input.seekg(0, std::ios::end);
  const std::streamsize file_size = input.tellg();
  input.seekg(0, std::ios::beg);

  if (file_size <= 0 || (file_size % static_cast<std::streamsize>(sizeof(Order))) != 0) {
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

} // namespace

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  const char *host = "127.0.0.1";
  uint16_t port = 8080;
  const char *benchmark_path = "benchmark_load.bin";

  if (argc >= 2)
    host = argv[1];
  if (argc >= 3)
    port = static_cast<uint16_t>(std::stoi(argv[2]));
  if (argc >= 4)
    benchmark_path = argv[3];

  const std::vector<Order> orders = LoadBenchmarkFile(benchmark_path);
  if (orders.empty())
    return 1;

  std::cout << "[Replay] Connecting to " << host << ":" << port
            << " | replaying " << orders.size() << " orders from "
            << benchmark_path << "\n";

  const int sock = Connect(host, port);
  if (sock < 0) {
    std::cerr << "[Replay] Connection failed\n";
    return 1;
  }

  size_t sent = 0;
  for (const Order &order : orders) {
    if (!SendFrame(sock, order)) {
      std::cerr << "[Replay] Send failed at order " << sent << "\n";
      close(sock);
      return 1;
    }
    ++sent;
  }

  if (!SendPoisonPill(sock)) {
    std::cerr << "[Replay] Failed to send poison pill\n";
    close(sock);
    return 1;
  }

  close(sock);
  std::cout << "[Replay] Complete. Orders sent: " << sent << "\n";
  return 0;
}
