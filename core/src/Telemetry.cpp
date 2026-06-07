#include "Constants.h"
#include "Hash.h"
#include "Telemetry.h"
#include <array>
#include <vector>

namespace {

std::array<std::vector<uint64_t>, SHARD_COUNT> g_trade_hashes;
} // namespace

namespace Telemetry {

void ReportTrade(uint16_t instrument_id, uint64_t maker_sequence_id,
                 uint64_t taker_sequence_id, uint32_t executed_quantity,
                 uint64_t execution_price) {
  if (instrument_id >= SHARD_COUNT) [[unlikely]]
    return;
  auto &ledger = g_trade_hashes[instrument_id];
  if (ledger.capacity() < 1'000'000)
    ledger.reserve(1'000'000);
  ledger.push_back(HashTrade(instrument_id, maker_sequence_id, taker_sequence_id,
                             executed_quantity, execution_price));
}

std::vector<uint64_t> GetTradeHashes(uint16_t instrument_id) {
  if (instrument_id >= SHARD_COUNT)
    return {};
  return g_trade_hashes[instrument_id];
}

void AppendTradeHash(uint16_t instrument_id, uint64_t hash) {
  if (instrument_id >= SHARD_COUNT) [[unlikely]]
    return;
  g_trade_hashes[instrument_id].push_back(hash);
}

uint64_t GetTotalTradeCount() {
  uint64_t total = 0;
  for (int i = 1; i <= NUM_INSTRUMENTS; ++i)
    total += g_trade_hashes[i].size();
  return total;
}

} // namespace Telemetry
