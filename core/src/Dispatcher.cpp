#include "Dispatcher.h"
#include "Artifacts.h"
#include "Constants.h"
#include <iostream>
#include <time.h>

namespace {

bool IsQueueOverflow(size_t depth, size_t max_depth) {
  return depth > max_depth;
}

uint64_t ElapsedNs(const timespec &start, const timespec &end) {
  return (end.tv_sec - start.tv_sec) * 1000000000ULL +
         (end.tv_nsec - start.tv_nsec);
}

} // namespace

void RunDispatcher(ExchangeEngine *engine, RunContext &ctx) {
  const bool benchmark = ctx.IsBenchmarkMode();
  const size_t max_queue_depth = ctx.MaxQueueDepth();
  const size_t reserve = benchmark ? 5'000'000 : 1'000'000;

  if (benchmark) {
    ctx.latencies.reserve(reserve);
  } else {
    ctx.input_ledger.reserve(reserve);
  }

  std::cout << "[Wrapper] Dispatcher awaiting orders..." << std::endl;

  const char *shutdown_reason = "graceful";
  bool have_window = false;
  timespec window_start{}, window_end{};

  while (true) {
    auto result = ctx.DequeueOrder();
    if (!result.order)
      break;

    if (IsQueueOverflow(result.queue_depth, max_queue_depth)) {
      std::cout << "\n[Wrapper FATAL] Queue Overflow! Backlog exceeded "
                << max_queue_depth << " orders. Tripping Breaker...\n";
      shutdown_reason = "queue_overflow";
      ctx.SignalShutdown();
      break;
    }

    const Order &order = *result.order;
    if (!benchmark)
      ctx.input_ledger.push_back(order);

    timespec start_ts{}, end_ts{};
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    engine->ProcessOrder(order);
    clock_gettime(CLOCK_MONOTONIC, &end_ts);

    if (!have_window) {
      window_start = start_ts;
      have_window = true;
    }
    window_end = end_ts;

    const uint64_t duration = ElapsedNs(start_ts, end_ts);
    if (duration > SLA_THRESHOLD_NS) {
      std::cout << "\n[Wrapper FATAL] SLA Breach Detected! Execution took "
                << duration << " ns. Tripping Circuit Breaker...\n";
      shutdown_reason = "sla_breach";
      ctx.SignalShutdown();
      break;
    }

    if (benchmark)
      ctx.latencies.push_back(duration);
  }

  const uint64_t processing_duration_ns =
      have_window ? ElapsedNs(window_start, window_end) : 0;
  Artifacts::FinalizeAndExit(engine, ctx, processing_duration_ns, shutdown_reason);
}
