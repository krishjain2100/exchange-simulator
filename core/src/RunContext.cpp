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

  max_queue_depth_ = IsBenchmarkMode() ? PHASE2_MAX_QUEUE_DEPTH
                                       : PHASE1_MAX_QUEUE_DEPTH;

  std::cout << "[Wrapper] Run mode: " << run_mode_
            << " | queue breaker at " << max_queue_depth_
            << " (ring capacity " << ORDER_QUEUE_CAPACITY << ")\n";
}

bool RunContext::IsBenchmarkMode() const {
  return run_mode_ != nullptr &&
         std::strcmp(run_mode_, RUN_MODE_BENCHMARK) == 0;
}

void RunContext::EnqueueOrder(Order order) {
  orders_received_.store(true);
  while (!order_queue_->try_enqueue(order)) {
    std::this_thread::yield();
  }
  queue_cv_.notify_one();
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
    queue_cv_.wait(lock, [this] {
      return !order_queue_->empty() || ingress_closed_.load() ||
             shutdown_.load();
    });
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

bool RunContext::ShouldStopAccepting() const {
  return ingress_closed_.load() || shutdown_.load();
}

void RunContext::OnClientConnected() { active_clients_.fetch_add(1); }

void RunContext::OnClientDisconnected() {
  const int remaining = active_clients_.fetch_sub(1) - 1;
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
