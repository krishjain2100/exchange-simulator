#pragma once
#include "Order.h"
#include <cstdint>
#include <vector>

class ExchangeEngine {
public:
  virtual ~ExchangeEngine() = default;
  virtual void Init() = 0;
  virtual void ProcessOrder(const Order &order) = 0;
  virtual std::vector<Order> GetRestingOrders(uint16_t instrument_id) const = 0;
};

ExchangeEngine *CreateEngine();