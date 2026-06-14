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
  // Total: 32 bytes per order — two orders fit exactly in one 64-byte cache line.
  uint64_t sequence_id;     // 8 bytes
  uint64_t price;           // 8 bytes (Overloaded with target_seq_id for CANCEL orders)
  uint32_t quantity;        // 4 bytes
  uint32_t client_id;       // 4 bytes
  uint16_t instrument_id;   // 2 bytes
  Side side;                // 1 byte
  OrderType type;           // 1 byte
  uint32_t _pad{0};         // 4 bytes — explicit padding for 32-byte cache-line alignment
};
#pragma pack(pop)

