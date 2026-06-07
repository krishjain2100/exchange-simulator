#pragma once
#include <cstddef>
#include <cstdint>

constexpr int NUM_INSTRUMENTS = 4;
constexpr int SHARD_COUNT = NUM_INSTRUMENTS + 1;

// Docker env HFT_RUN_MODE — keep in sync with worker STAGE constants.
constexpr const char *RUN_MODE_CORRECTNESS = "correctness";
constexpr const char *RUN_MODE_BENCHMARK = "benchmark";

// Per-order SLA enforced by the dispatcher circuit breaker (1s).
constexpr uint64_t SLA_THRESHOLD_NS = 1'000'000'000;

// Ring buffer capacity (compile-time). Must exceed both phase thresholds.
constexpr size_t ORDER_QUEUE_CAPACITY = 1 << 23; // 8,388,608

// Circuit-breaker backlog limits (runtime, via HFT_RUN_MODE).
constexpr size_t PHASE1_MAX_QUEUE_DEPTH = 1'000'000;
constexpr size_t PHASE2_MAX_QUEUE_DEPTH = 6'000'000;

static_assert(PHASE1_MAX_QUEUE_DEPTH < ORDER_QUEUE_CAPACITY,
              "Phase 1 threshold must fit inside the ring buffer");
static_assert(PHASE2_MAX_QUEUE_DEPTH < ORDER_QUEUE_CAPACITY,
              "Phase 2 threshold must fit inside the ring buffer");
