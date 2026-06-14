#include "Dispatcher.h"
#include "Artifacts.h"
#include "Constants.h"
#include "Telemetry.h"
#include <algorithm>
#include <cstdint>
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

uint64_t TimespecToNs(const timespec &ts) {
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Peak 1-second order rate observed before shutdown.
struct PeakOpsTracker {
  uint64_t window_start_ns = 0;
  uint64_t orders_in_window = 0;
  uint64_t max_ops = 0;

  void CloseWindow(uint64_t now_ns) {
    if (window_start_ns == 0)
      return;

    const uint64_t elapsed = now_ns - window_start_ns;
    // Only full 1-second windows count — partial tails (e.g. queue drain)
    // would otherwise inflate peak ops far above total orders.
    if (elapsed < 1'000'000'000ULL)
      return;

    const uint64_t rate = orders_in_window * 1'000'000'000ULL / elapsed;
    max_ops = std::max(max_ops, rate);
  }

  void OnOrderProcessed(uint64_t now_ns) {
    if (window_start_ns == 0)
      window_start_ns = now_ns;

    orders_in_window++;

    const uint64_t elapsed = now_ns - window_start_ns;
    if (elapsed >= 1'000'000'000ULL) {
      CloseWindow(now_ns);
      window_start_ns = now_ns;
      orders_in_window = 0;
    }
  }

  void Finalize(uint64_t now_ns) {
    CloseWindow(now_ns);
  }
};

} // namespace

void RunDispatcher(ExchangeEngine *engine, RunContext &ctx) {
  const bool benchmark = ctx.IsBenchmarkMode();
  const size_t max_queue_depth = ctx.MaxQueueDepth();
  const size_t reserve = benchmark ? BENCHMARK_LATENCY_CAP : 32'000'000;

  if (benchmark) {
    ctx.latencies.reserve(reserve);
  } else {
    ctx.input_ledger.reserve(reserve);
  }

  std::cout << "[Wrapper] Dispatcher awaiting orders..." << std::endl;

  const char *shutdown_reason = "graceful";
  bool have_window = false;
  timespec window_start{}, window_end{};
  PeakOpsTracker peak_ops;

  uint32_t check_counter = 0;
  uint32_t consecutive_breaches = 0;

  while (true) {
    auto result = ctx.DequeueOrder();
    if (!result.order)
      break;

    if (!benchmark && IsQueueOverflow(result.queue_depth, max_queue_depth)) {
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

    if (benchmark) {
      if (++check_counter % 1000 == 0) {
        const bool q_breached = (result.queue_depth > BENCHMARK_FAILURE_QUEUE_DEPTH);
        const bool l_breached = (duration > BENCHMARK_FAILURE_PROCESS_TIME_NS);
        const bool breached = q_breached || l_breached;

        if (breached) {
          if (++consecutive_breaches >= 3) {
            const char *reason = "unknown";
            if (q_breached && l_breached) {
              reason = "queue_depth and latency exceeded";
            } else if (q_breached) {
              reason = "queue_depth exceeded";
            } else if (l_breached) {
              reason = "latency exceeded";
            }
            std::cout << "\n[Wrapper FATAL] Health Breach (" << reason << ")! Queue depth: "
                      << result.queue_depth << " | Latency: " << duration << " ns" << std::endl;
            shutdown_reason = "health_breach";
            ctx.SignalHealthBreach();
            break;
          }
        } else {
          consecutive_breaches = 0;
        }
      }
    }

    if (!benchmark && duration > SLA_THRESHOLD_NS) {
      std::cout << "\n[Wrapper FATAL] SLA Breach Detected! Execution took "
                << duration << " ns. Tripping Circuit Breaker...\n";
      shutdown_reason = "sla_breach";
      ctx.SignalShutdown();
      break;
    }

    if (benchmark && ctx.latencies.size() < BENCHMARK_LATENCY_CAP)
      ctx.latencies.push_back(duration);

    peak_ops.OnOrderProcessed(TimespecToNs(end_ts));
  }

  if (have_window) {
    peak_ops.Finalize(TimespecToNs(window_end));
  }

  const uint64_t processing_duration_ns =
      have_window ? ElapsedNs(window_start, window_end) : 0;

  if (benchmark) {
    const uint64_t trades_executed = Telemetry::GetTotalTradeCount();
    const uint64_t orders_processed = ctx.latencies.size();
    Artifacts::FinalizeProbe(orders_processed, trades_executed, processing_duration_ns,
                             peak_ops.max_ops, shutdown_reason, ctx.latencies);
    return;
  }

  Artifacts::FinalizeAndExit(engine, ctx, processing_duration_ns, shutdown_reason,
                             peak_ops.max_ops);
}
