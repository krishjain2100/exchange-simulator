#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <algorithm>
#include <iostream>
#include <vector>

// The "Naive" implementation standard developers would write
class SlowEngine : public ExchangeEngine {
private:
  // DEADLY SIN 1: A flat vector for the entire book.
  // No O(1) array indexing by instrument.
  std::vector<Order> bids;
  std::vector<Order> asks;

  void HandleCancel(const Order &cancel_order) {
    // DEADLY SIN 2: O(N) Linear Search for Cancels
    uint64_t target_id = cancel_order.price; // Target seq overloaded in price

    if (cancel_order.side == Side::BUY) {
      for (auto it = bids.begin(); it != bids.end(); ++it) {
        if (it->sequence_id == target_id) {
          // DEADLY SIN 3: O(N) Vector Erase
          // This forces the CPU to physically shift thousands of orders in RAM
          bids.erase(it);
          return;
        }
      }
    } else {
      for (auto it = asks.begin(); it != asks.end(); ++it) {
        if (it->sequence_id == target_id) {
          asks.erase(it);
          return;
        }
      }
    }
  }

  bool WouldSelfMatch(const Order &incoming) {
    if (incoming.side == Side::BUY) {
      for (const auto &ask : asks) {
        if (ask.instrument_id != incoming.instrument_id)
          continue;
        if (incoming.type == OrderType::LIMIT && incoming.price < ask.price)
          continue;
        if (ask.client_id == incoming.client_id)
          return true;
      }
    } else {
      for (const auto &bid : bids) {
        if (bid.instrument_id != incoming.instrument_id)
          continue;
        if (incoming.type == OrderType::LIMIT && incoming.price > bid.price)
          continue;
        if (bid.client_id == incoming.client_id)
          return true;
      }
    }
    return false;
  }

public:
  void Init() override {
    std::cout << "[SlowEngine] Booting unoptimized baseline engine..."
              << std::endl;
    bids.reserve(100000);
    asks.reserve(100000);
  }

  void ProcessOrder(const Order &order) override {
    if (order.type == OrderType::CANCEL) {
      HandleCancel(order);
      return;
    }

    if (WouldSelfMatch(order))
      return;

    Order current = order;

    if (current.side == Side::BUY) {
      // Must re-sort every time because std::vector doesn't auto-sort like
      // std::map
      std::sort(asks.begin(), asks.end(), [](const Order &a, const Order &b) {
        if (a.price == b.price)
          return a.sequence_id < b.sequence_id; // Price-Time
        return a.price < b.price;               // Lowest ask first
      });

      auto it = asks.begin();
      while (it != asks.end() && current.quantity > 0) {
        if (it->instrument_id != current.instrument_id) {
          ++it;
          continue;
        }

        if (current.type == OrderType::LIMIT && current.price < it->price)
          break;

        uint32_t match_qty = std::min(current.quantity, it->quantity);
        Telemetry::ReportTrade(current.instrument_id, it->sequence_id,
                               current.sequence_id, match_qty, it->price);

        current.quantity -= match_qty;
        it->quantity -= match_qty;

        if (it->quantity == 0) {
          it = asks.erase(it); // O(N) memory shift on the hot path!
        } else {
          ++it;
        }
      }

      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        bids.push_back(current);
      }

    } else if (current.side == Side::SELL) {
      std::sort(bids.begin(), bids.end(), [](const Order &a, const Order &b) {
        if (a.price == b.price)
          return a.sequence_id < b.sequence_id;
        return a.price > b.price; // Highest bid first
      });

      auto it = bids.begin();
      while (it != bids.end() && current.quantity > 0) {
        if (it->instrument_id != current.instrument_id) {
          ++it;
          continue;
        }

        if (current.type == OrderType::LIMIT && current.price > it->price)
          break;

        uint32_t match_qty = std::min(current.quantity, it->quantity);
        Telemetry::ReportTrade(current.instrument_id, it->sequence_id,
                               current.sequence_id, match_qty, it->price);

        current.quantity -= match_qty;
        it->quantity -= match_qty;

        if (it->quantity == 0) {
          it = bids.erase(it);
        } else {
          ++it;
        }
      }

      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        asks.push_back(current);
      }
    }
  }

  std::vector<Order> GetRestingOrders() const override {
    std::vector<Order> resting;
    for (const auto &b : bids)
      resting.push_back(b);
    for (const auto &a : asks)
      resting.push_back(a);
    return resting;
  }
};

ExchangeEngine *CreateEngine() { return new SlowEngine(); }