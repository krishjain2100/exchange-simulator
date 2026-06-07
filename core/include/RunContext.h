#pragma once
#include "Constants.h"
#include "MpscRingBuffer.h"
#include "Order.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Shared runtime state between the network ingress and order-dispatch threads.
class RunContext {
public:
  RunContext();

  struct DequeueResult {
    std::optional<Order> order;
    size_t queue_depth = 0;
  };

  void EnqueueOrder(Order order);
  DequeueResult DequeueOrder();

  // Bots finished sending — close ingress, dispatcher drains the queue first.
  void SignalIngressComplete();
  // Circuit breaker — stop ingress immediately (dispatcher may abandon backlog).
  void SignalShutdown();
  bool ShouldStopAccepting() const;

  void OnClientConnected();
  void OnClientDisconnected();

  void SetServerFd(int fd);
  void ShutdownServerSocket();

  size_t MaxQueueDepth() const { return max_queue_depth_; }
  const char *RunMode() const { return run_mode_; }
  bool IsBenchmarkMode() const;

  std::vector<Order> input_ledger;
  std::vector<uint64_t> latencies;

private:
  size_t max_queue_depth_{PHASE1_MAX_QUEUE_DEPTH};
  const char *run_mode_{"correctness"};
  std::unique_ptr<MpscRingBuffer<Order, ORDER_QUEUE_CAPACITY>> order_queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::atomic<bool> ingress_closed_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<int> active_clients_{0};
  std::atomic<bool> orders_received_{false};
  int server_fd_{-1};
};
