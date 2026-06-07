#pragma once
#include "Order.h"
#include <cstddef>

struct ParsedMessage {
  enum class Kind { Order, PoisonPill, Invalid };

  Kind kind = Kind::Invalid;
  Order order{};
};

// Parse one fixed-size binary Order frame from the TCP stream.
// data/len must be exactly sizeof(Order); no heap allocation.
ParsedMessage ParseOrderFrame(const char *data, size_t len);
