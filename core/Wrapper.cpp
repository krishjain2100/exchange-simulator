#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <queue>
#include <sched.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

// Queue Data Structures
std::queue<Order> order_queue;
std::mutex queue_mutex;
std::condition_variable queue_cv;
std::atomic<bool> load_test_complete{false};

struct TradeRecord {
  uint16_t instrument_id;
  uint64_t maker_id, taker_id;
  uint32_t qty;
  uint64_t price;
};

std::vector<Order> input_ledger;
std::vector<uint64_t> latencies;
std::vector<TradeRecord> trade_ledger;

// SLA Threshold for order processing (500 milliseconds in nanoseconds)
constexpr uint64_t SLA_THRESHOLD_NS = 500000000;

// Telemetry
namespace Telemetry {
void ReportTrade(uint16_t inst_id, uint64_t maker_id, uint64_t taker_id,
                 uint32_t qty, uint64_t price) {
  trade_ledger.push_back({inst_id, maker_id, taker_id, qty, price});
}
} // namespace Telemetry

void Shutdown(ExchangeEngine *engine, std::vector<uint64_t> &latencies) {
  //  Shutdown Sequence
  std::cout << "[Wrapper] Engine shutdown triggered. Flushing state to disk..."
            << std::endl;

  // 1. Flush Ledgers to CSVs
  std::ofstream input_file("input_ledger.csv");
  input_file << "SequenceID,ClientID,InstrumentID,Type,Side,Price,Qty\n";
  for (const auto &o : input_ledger) {
    input_file << o.sequence_id << "," << o.client_id << "," << o.instrument_id
               << "," << static_cast<int>(o.type) << ","
               << static_cast<int>(o.side) << "," << o.price << ","
               << o.quantity << "\n";
  }
  input_file.close();

  // 2. Flush Trades
  std::ofstream trade_file("trade_ledger.csv");
  trade_file << "InstrumentID,MakerID,TakerID,Qty,Price\n";
  for (const auto &t : trade_ledger) {
    trade_file << t.instrument_id << "," << t.maker_id << "," << t.taker_id
               << "," << t.qty << "," << t.price << "\n";
  }
  trade_file.close();

  // 3. Flush Final Book State
  std::ofstream book_file("final_book.csv");
  book_file << "SequenceID,ClientID,InstrumentID,Type,Side,Price,Qty\n";
  std::vector<Order> resting = engine->GetRestingOrders();
  for (const auto &o : resting) {
    book_file << o.sequence_id << "," << o.client_id << "," << o.instrument_id
              << "," << static_cast<int>(o.type) << ","
              << static_cast<int>(o.side) << "," << o.price << "," << o.quantity
              << "\n";
  }
  book_file.close();

  uint64_t p50 = 0, p90 = 0, p99 = 0;

  if (!latencies.empty()) {
    std::sort(latencies.begin(), latencies.end());
    size_t count = latencies.size();
    p50 = latencies[count * 0.50];
    p90 = latencies[count * 0.90];
    p99 = latencies[count * 0.99];
  }

  // Dump to file as comma-separated values
  std::ofstream metrics_file("latency.txt");
  metrics_file << p50 << "," << p90 << "," << p99;
  metrics_file.close();

  std::cout << "[Wrapper] Latency (ns) -> p50: " << p50 << " | p90: " << p90
            << " | p99: " << p99 << std::endl;
  std::cout << "[Wrapper] CSVs generated: input_ledger.csv, trade_ledger.csv, "
               "final_book.csv, latency.txt\n";
  std::cout << "========================================\n";
}

void PinThreadToCore(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pthread_t current_thread = pthread_self();
  int result =
      pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

  if (result != 0) {
    std::cerr << "[Warning] Failed to pin thread to Virtual Core " << core_id
              << "\n";
  } else {
    std::cout << "[Optimal] Dispatcher locked to Linux Virtual Core " << core_id
              << "\n";
  }
}

void DispatcherLoop(ExchangeEngine *engine) {
  std::cout << "[Wrapper] Dispatcher thread started." << std::endl;

  PinThreadToCore(1);

  input_ledger.reserve(5000000);
  trade_ledger.reserve(500000);
  latencies.reserve(500000);

  while (true) {
    Order current_order;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_cv.wait(lock, [] {
        return !order_queue.empty() || load_test_complete.load();
      });

      if (load_test_complete.load() && order_queue.empty())
        break;
      current_order = order_queue.front();
      order_queue.pop();
    }

    // 1. Record input order in ledger
    input_ledger.push_back(current_order);

    // 2. Process order and measure latency
    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    engine->ProcessOrder(current_order);
    clock_gettime(CLOCK_MONOTONIC, &end_ts);

    uint64_t duration = (end_ts.tv_sec - start_ts.tv_sec) * 1000000000ULL +
                        (end_ts.tv_nsec - start_ts.tv_nsec);

    if (duration > SLA_THRESHOLD_NS) {
      std::cerr << "\n[Wrapper FATAL] SLA Breach Detected!" << std::endl;
      std::cerr << "[Wrapper FATAL] Order took " << duration
                << " ns (Limit: " << SLA_THRESHOLD_NS << " ns)." << std::endl;
      std::cerr << "[Wrapper FATAL] Algorithmic execution too slow. "
                   "Terminating engine immediately."
                << std::endl;
      _exit(1);
    }

    latencies.push_back(duration);
  }

  Shutdown(engine, latencies);
}

//  The TCP Client Session
void ClientSession(int client_sock) {
  char buffer[4096];
  std::string stream_buffer = "";

  while (true) {
    ssize_t bytes_received = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0)
      break;

    buffer[bytes_received] = '\0';

    stream_buffer += buffer;

    size_t pos;
    while ((pos = stream_buffer.find('\n')) != std::string::npos) {

      std::string json_str = stream_buffer.substr(0, pos);
      stream_buffer.erase(0, pos + 1);

      if (json_str.empty())
        continue;

      uint64_t ingress_ns = 0;
      uint64_t s, p;
      uint32_t c, i, t, q, side;

      int parsed_count =
          sscanf(json_str.c_str(),
                 "{\"s\": %" SCNu64
                 ", \"c\": %u, \"i\": %u, \"t\": %u, \"p\": %" SCNu64
                 ", \"q\": %u, \"side\": %u}",
                 &s, &c, &i, &t, &p, &q, &side);

      if (parsed_count == 7) {
        Order new_order;
        new_order.sequence_id = s;
        new_order.timestamp_ns = ingress_ns;
        new_order.instrument_id = static_cast<uint16_t>(i);
        new_order.type = static_cast<OrderType>(t);
        new_order.side = static_cast<Side>(side);
        new_order.client_id = c;

        if (new_order.type == OrderType::CANCEL) {
          new_order.price = p;
          new_order.quantity = 0;
        } else {
          new_order.price = p;
          new_order.quantity = q;
        }
        {
          std::lock_guard<std::mutex> lock(queue_mutex);
          order_queue.push(new_order);
        }
        queue_cv.notify_one();
      }
    }
  }
  close(client_sock);
}

//  The TCP Listener Thread
void TCPServer() {

  PinThreadToCore(0);
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == 0) {
    perror("[Server ERROR] Socket creation failed");
    _exit(1);
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(8080);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("[Server ERROR] Bind failed. Port 8080 is likely still in use!");
    _exit(1);
  }

  if (listen(server_fd, 100) < 0) {
    perror("[Server ERROR] Listen failed");
    _exit(1);
  }

  std::cout << "[Wrapper] Listening on Port 8080. Awaiting Bot Fleet..."
            << std::endl;

  // 1 connection for Phase 1 + 18 connections for the Threaded Bots
  static constexpr int FLEET_EXPECTED_BOTS = 19;
  int bots_connected = 0;

  std::vector<std::thread> client_threads;

  while (bots_connected < FLEET_EXPECTED_BOTS) {
    int new_socket = accept(server_fd, nullptr, nullptr);
    if (new_socket < 0)
      continue;
    std::cout << "[Wrapper] Bot " << ++bots_connected << " connected!"
              << std::endl;
    client_threads.emplace_back(ClientSession, new_socket);
  }

  for (auto &th : client_threads) {
    if (th.joinable())
      th.join();
  }

  std::cout << "[Wrapper] All bots disconnected. Initiating sequence..."
            << std::endl;
  close(server_fd);
  load_test_complete.store(true);
  queue_cv.notify_all();
}

int main() {
  ExchangeEngine *engine = CreateEngine();
  engine->Init();

  std::thread dispatcher_thread(DispatcherLoop, engine);
  std::thread network_thread(TCPServer);

  network_thread.join();
  dispatcher_thread.join();

  delete engine;
  return 0;
}