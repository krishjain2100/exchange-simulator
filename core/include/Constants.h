#pragma once
#include <cstddef>
#include <cstdint>

constexpr int NUM_INSTRUMENTS = 4;
constexpr int SHARD_COUNT = NUM_INSTRUMENTS + 1;

// Docker env run mode — keep in sync with worker STAGE constants.
constexpr const char *RUN_MODE_CORRECTNESS = "correctness";
constexpr const char *RUN_MODE_BENCHMARK = "benchmark";

// Per-order SLA enforced by the dispatcher circuit breaker (1s).
constexpr uint64_t SLA_THRESHOLD_NS = 1'000'000'000;

// Ring buffer capacity (compile-time). Must exceed both phase thresholds.
constexpr size_t ORDER_QUEUE_CAPACITY = 1 << 23; // 8,388,608

// Circuit-breaker backlog limit (correctness mode only).
constexpr size_t PHASE1_MAX_QUEUE_DEPTH = 1'000'000;

// Cap stored per-order latencies in benchmark mode to limit memory growth.
constexpr size_t BENCHMARK_LATENCY_CAP = 2'000'000;