#pragma once
#include <cstdint>

namespace Telemetry {
void ReportTrade(uint16_t instrument_id, uint64_t maker_sequence_id,
                 uint64_t taker_sequence_id, uint32_t executed_quantity,
                 uint64_t execution_price);
}