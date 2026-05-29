#pragma once
#include <cstdint>

#pragma pack(push, 1)
// This tells the compiler, "Save your current alignment settings (push), and
// change the new alignment rule to 1 byte." A 1-byte alignment rule means the
// compiler is forbidden from inserting any implicit padding. It must place
// every variable immediately after the previous one.

enum class Side : uint8_t { BUY = 1, SELL = 2 };
enum class OrderType : uint8_t { LIMIT = 1, MARKET = 2, CANCEL = 3 };

struct Order {
  // Total: 32 bytes per order
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
// This tells the compiler, "Restore the alignment settings you saved earlier."
// You do this at the end of your struct so you don't accidentally ruin the
// memory alignment of standard library headers included later in the file.