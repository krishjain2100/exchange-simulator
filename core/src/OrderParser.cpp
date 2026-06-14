#include "OrderParser.h"
#include <cstring>

static_assert(sizeof(Order) == 32, "Wire frame must stay 32 bytes");

namespace {

bool IsOrderType(OrderType type) {
  return type == OrderType::LIMIT || type == OrderType::MARKET ||  type == OrderType::CANCEL;
}

} // namespace

ParsedMessage ParseOrderFrame(const char *data, size_t len) {
  ParsedMessage result;
  if (len != sizeof(Order))
    return result;

  std::memcpy(&result.order, data, sizeof(Order));

  if (result.order.type == OrderType::POISON) {
    result.kind = ParsedMessage::Kind::PoisonPill;
    return result;
  }

  if (!IsOrderType(result.order.type))
    return result;

  result.kind = ParsedMessage::Kind::Order;
  return result;
}
