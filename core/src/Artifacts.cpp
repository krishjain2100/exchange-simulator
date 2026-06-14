#include "Artifacts.h"
#include "Constants.h"
#include "Hash.h"
#include "RunContext.h"
#include "Telemetry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace Artifacts {

void WriteInputLedger(const std::vector<Order> &ledger) {
  FILE *input_file = fopen("input_ledger.bin", "wb");
  fwrite(ledger.data(), sizeof(Order), ledger.size(), input_file);
  fclose(input_file);
}

void WriteVerificationPayload(ExchangeEngine *engine) {
  FILE *payload_file = fopen("payload.bin", "wb");

  for (int i = 1; i <= NUM_INSTRUMENTS; ++i) {
    std::vector<Order> resting_for_inst = engine->GetRestingOrders(i);
    uint64_t final_book_hash = HashSortedBook(resting_for_inst);

    Telemetry::AppendTradeHash(i, final_book_hash);
    auto hashes = Telemetry::GetTradeHashes(i);
    uint64_t count = hashes.size();

    fwrite(&count, sizeof(uint64_t), 1, payload_file);
    if (count) fwrite(hashes.data(), sizeof(uint64_t), count, payload_file);
  }
  fclose(payload_file);
}

void WriteThroughputMetrics(uint64_t orders_processed, uint64_t trades_executed,
                            uint64_t processing_duration_ns, uint64_t max_ops,
                            const char *shutdown_reason) {
  std::ofstream throughput_file("phase1_throughput.txt");
  throughput_file << orders_processed << "," << trades_executed << ","
                  << processing_duration_ns << "," << max_ops << ","
                  << shutdown_reason << ",0,0,0\n";
  throughput_file.close();

  std::cout << "[Wrapper] Throughput -> orders: " << orders_processed
            << " | trades: " << trades_executed
            << " | max_ops: " << max_ops
            << " | shutdown: " << shutdown_reason << std::endl;
}

void WriteBenchmarkMetrics(uint64_t orders_processed, uint64_t trades_executed,
                           uint64_t processing_duration_ns, uint64_t max_ops,
                           const char *shutdown_reason,
                           const std::vector<uint64_t> &latencies) {
  uint64_t p50 = 0, p90 = 0, p99 = 0;
  if (!latencies.empty()) {
    std::vector<uint64_t> sorted = latencies;
    std::sort(sorted.begin(), sorted.end());
    auto quantile = [&](double q) -> uint64_t {
      size_t n = sorted.size();
      size_t idx = static_cast<size_t>(std::floor(q * static_cast<double>(n)));
      if (idx >= n) idx = n - 1;
      return sorted[idx];
    };
    p50 = quantile(0.50);
    p90 = quantile(0.90);
    p99 = quantile(0.99);
  }

  std::ofstream metrics_file("phase2_metrics.txt");
  metrics_file << orders_processed << "," << trades_executed << ","
               << processing_duration_ns << "," << max_ops << ","
               << shutdown_reason << "," << p50 << "," << p90 << "," << p99
               << "\n";
  metrics_file.close();

  std::cout << "[Wrapper] Throughput -> orders: " << orders_processed
            << " | trades: " << trades_executed
            << " | max_ops: " << max_ops
            << " | shutdown: " << shutdown_reason << std::endl;
  std::cout << "[Wrapper] Latency (ns) -> p50: " << p50 << " | p90: " << p90
            << " | p99: " << p99 << std::endl;
}

void FinalizeProbe(uint64_t orders_processed, uint64_t trades_executed,
                   uint64_t processing_duration_ns, uint64_t max_ops,
                   const char *shutdown_reason,
                   const std::vector<uint64_t> &latencies) {
  WriteBenchmarkMetrics(orders_processed, trades_executed, processing_duration_ns,
                        max_ops, shutdown_reason, latencies);
  std::cout << "[Wrapper] Probe ready\n" << std::flush;
}

void FinalizeAndExit(ExchangeEngine *engine, RunContext &ctx,
                     uint64_t processing_duration_ns, const char *shutdown_reason,
                     uint64_t max_ops) {
  const bool benchmark = ctx.IsBenchmarkMode();
  const uint64_t trades_executed = Telemetry::GetTotalTradeCount();
  const uint64_t orders_processed = benchmark ? ctx.latencies.size() : ctx.input_ledger.size();

  std::cout << "[Wrapper] Swarm retreated. Flushing "
            << (benchmark ? "benchmark metrics" : "verification artifacts")
            << " to SSD...\n";

  if (benchmark) {
    WriteBenchmarkMetrics(orders_processed, trades_executed, processing_duration_ns,
                          max_ops, shutdown_reason, ctx.latencies);
  } else {
    WriteThroughputMetrics(orders_processed, trades_executed, processing_duration_ns,
                           max_ops, shutdown_reason);
    WriteInputLedger(ctx.input_ledger);
    WriteVerificationPayload(engine);
  }

  std::cout << "========================================\n";
  _exit(0);
}

} // namespace Artifacts
