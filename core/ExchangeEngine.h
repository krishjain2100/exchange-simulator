#pragma once
#include <cstdint>
#include <vector>
#include "Order.h"

class ExchangeEngine {
public:
    virtual ~ExchangeEngine() = default;
    virtual void Init() = 0;
    virtual void ProcessOrder(const Order& order) = 0;
    
    // Returns all unmatched orders remaining in the engine's memory
    virtual std::vector<Order> GetRestingOrders() const = 0; 
};

ExchangeEngine* CreateEngine();