#pragma once
#include "ExchangeEngine.h"
#include "Order.h"
#include "RunContext.h"
#include <cstdint>
#include <vector>

namespace Artifacts {

void WriteInputLedger(const std::vector<Order> &ledger);
void WriteVerificationPayload(ExchangeEngine *engine);
void WriteThroughputMetrics(uint64_t orders_processed, uint64_t trades_executed,
                            uint64_t processing_duration_ns, uint64_t max_ops,
                            const char *shutdown_reason);
void WriteBenchmarkMetrics(uint64_t orders_processed, uint64_t trades_executed,
                           uint64_t processing_duration_ns, uint64_t max_ops,
                           const char *shutdown_reason,
                           const std::vector<uint64_t> &latencies);

// Phase 2: write per-probe metrics and signal readiness for the next bracket.
void FinalizeProbe(uint64_t orders_processed, uint64_t trades_executed,
                   uint64_t processing_duration_ns, uint64_t max_ops,
                   const char *shutdown_reason,
                   const std::vector<uint64_t> &latencies);

// Phase 1 (correctness): input_ledger.bin + payload.bin + phase1_throughput.txt
// Phase 2 (benchmark): phase2_metrics.txt — orders,trades,duration,max_ops,reason,p50,p90,p99
void FinalizeAndExit(ExchangeEngine *engine, RunContext &ctx,
                     uint64_t processing_duration_ns, const char *shutdown_reason,
                     uint64_t max_ops);

} // namespace Artifacts
