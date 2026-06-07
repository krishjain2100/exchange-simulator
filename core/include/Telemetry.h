#pragma once
#include <cstdint>
#include <vector>

namespace Telemetry {
void ReportTrade(uint16_t instrument_id, uint64_t maker_sequence_id,
                 uint64_t taker_sequence_id, uint32_t executed_quantity,
                 uint64_t execution_price);

std::vector<uint64_t> GetTradeHashes(uint16_t instrument_id);
void AppendTradeHash(uint16_t instrument_id, uint64_t hash);
uint64_t GetTotalTradeCount();
} // namespace Telemetry