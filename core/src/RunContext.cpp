#include "RunContext.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

RunContext::RunContext()
    : order_queue_(std::make_unique<MpscRingBuffer<Order, ORDER_QUEUE_CAPACITY>>()) {
  const char *env_mode = std::getenv("HFT_RUN_MODE");
  if (env_mode != nullptr && env_mode[0] != '\0')
    run_mode_ = env_mode;

  if (!IsBenchmarkMode())
    max_queue_depth_ = PHASE1_MAX_QUEUE_DEPTH;

  std::cout << "[Wrapper] Run mode: " << run_mode_;
  if (!IsBenchmarkMode())
    std::cout << " | queue breaker at " << max_queue_depth_;
  std::cout << " (ring capacity " << ORDER_QUEUE_CAPACITY << ")\n";
}

bool RunContext::IsBenchmarkMode() const {
  return run_mode_ != nullptr &&
         std::strcmp(run_mode_, RUN_MODE_BENCHMARK) == 0;
}

void RunContext::EnqueueOrder(Order order) {
  orders_received_.store(true);
  while (!order_queue_->try_enqueue(order)) {
    if (shutdown_.load())
      return;
    std::this_thread::yield();
  }
  if (dispatcher_waiting_.load(std::memory_order_acquire)) {
    queue_cv_.notify_one();
  }
}

RunContext::DequeueResult RunContext::DequeueOrder() {
  DequeueResult result;
  Order order;

  while (true) {
    if (order_queue_->try_dequeue(order)) {
      result.order = order;
      result.queue_depth = order_queue_->size() + 1;
      return result;
    }

    if ((ingress_closed_.load() || shutdown_.load()) && order_queue_->empty())
      return result;

    std::unique_lock<std::mutex> lock(queue_mutex_);
    dispatcher_waiting_.store(true, std::memory_order_release);
    queue_cv_.wait(lock, [this] {
      return !order_queue_->empty() || ingress_closed_.load() ||
             shutdown_.load();
    });
    dispatcher_waiting_.store(false, std::memory_order_release);
  }
}

void RunContext::SignalIngressComplete() {
  ingress_closed_.store(true);
  ShutdownServerSocket();
  queue_cv_.notify_all();
}

void RunContext::SignalShutdown() {
  shutdown_.store(true);
  ingress_closed_.store(true);
  ShutdownServerSocket();
  queue_cv_.notify_all();
}

void RunContext::SignalHealthBreach() {
  health_breached_.store(true);
  SignalShutdown();
}

bool RunContext::ShouldStopAccepting() const {
  return ingress_closed_.load() || shutdown_.load();
}

void RunContext::OnClientConnected() { active_clients_.fetch_add(1); }

void RunContext::OnClientDisconnected() {
  active_clients_.fetch_sub(1);
  // Benchmark runs multiple replay probes; only the poison pill ends the phase.
  if (IsBenchmarkMode())
    return;

  const int remaining = active_clients_.load();
  if (orders_received_.load() && remaining == 0) {
    std::cout << "\n[Wrapper] All Bot Nodes have disconnected. Draining "
                 "remaining queue...\n";
    SignalIngressComplete();
    if (server_fd_ >= 0)
      close(server_fd_);
  }
}

void RunContext::SetServerFd(int fd) { server_fd_ = fd; }

void RunContext::ShutdownServerSocket() {
  if (server_fd_ >= 0)
    shutdown(server_fd_, SHUT_RDWR);
}

void RunContext::ResetForNextProbe() {
  order_queue_ = std::make_unique<MpscRingBuffer<Order, ORDER_QUEUE_CAPACITY>>();
  ingress_closed_.store(false);
  shutdown_.store(false);
  health_breached_.store(false);
  active_clients_.store(0);
  orders_received_.store(false);
  dispatcher_waiting_.store(false);
  server_fd_ = -1;
  latencies.clear();
  input_ledger.clear();
}
