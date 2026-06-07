#include "Hash.h"
#include <algorithm>

uint64_t HashTrade(uint16_t instrument_id, uint64_t maker_sequence_id,
                   uint64_t taker_sequence_id, uint32_t executed_quantity,
                   uint64_t execution_price) {
  uint64_t h = instrument_id;
  h ^= maker_sequence_id + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= taker_sequence_id + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= executed_quantity + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= execution_price + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t HashSortedBook(std::vector<Order> &resting_orders) {
  std::sort(resting_orders.begin(), resting_orders.end(),
            [](const Order &a, const Order &b) {
              return a.sequence_id < b.sequence_id;
            });

  uint64_t hash = 14695981039346656037ULL;
  for (const auto &o : resting_orders) {
    hash ^= o.sequence_id;
    hash *= 1099511628211ULL;
    hash ^= o.client_id;
    hash *= 1099511628211ULL;
    hash ^= o.instrument_id;
    hash *= 1099511628211ULL;
    hash ^= static_cast<uint64_t>(o.side);
    hash *= 1099511628211ULL;
    hash ^= o.price;
    hash *= 1099511628211ULL;
    hash ^= o.quantity;
    hash *= 1099511628211ULL;
  }
  return hash;
}
