#pragma once
#include <cstdint>

namespace TelemetryChannel {

// Fire UDP sample to the worker during benchmark (Phase 2).
void InitFromEnvironment();
void SendSample(uint64_t sequence_id, uint32_t queue_depth,
                uint64_t process_time_ns);

} // namespace TelemetryChannel
