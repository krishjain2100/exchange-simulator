#pragma once
#include <cstdint>

#pragma pack(push, 1)
enum class Side : uint8_t { BUY = 1, SELL = 2 };
enum class OrderType : uint8_t {
  LIMIT = 1,
  MARKET = 2,
  CANCEL = 3,
  POISON = 9, // Wire-only shutdown sentinel (never reaches the engine book)
};

struct Order {
  // Total: 36 bytes per order — wire format for TCP frames and input_ledger.bin.
  uint64_t sequence_id;  // 8 bytes
  uint64_t timestamp_ns; // 8 bytes
  uint64_t price; // 8 bytes (Overloaded with target_seq_id for CANCEL orders)
  uint32_t quantity;      // 4 bytes
  uint32_t client_id;     // 4 bytes
  uint16_t instrument_id; // 2 bytes
  Side side;              // 1 byte
  OrderType type;         // 1 byte
};
#pragma pack(pop)
