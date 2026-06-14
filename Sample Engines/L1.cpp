#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <vector>

// The "Naive" implementation standard developers would write
class SlowEngine : public ExchangeEngine {
private:
  // DEADLY SIN 1: A flat vector for the entire book.
  // No O(1) array indexing by instrument.
  std::vector<Order> bids;
  std::vector<Order> asks;
  std::unordered_set<uint64_t> seen_sequences;

  void HandleCancel(const Order &cancel_order) {
    const uint64_t target_id = cancel_order.price;

    auto erase_target = [&](std::vector<Order> &book) -> bool {
      for (auto it = book.begin(); it != book.end(); ++it) {
        if (it->sequence_id == target_id) {
          book.erase(it);
          return true;
        }
      }
      return false;
    };

    if (erase_target(bids) || erase_target(asks))
      return;
  }

  bool WouldSelfMatch(const Order &incoming) {
    uint32_t qty_to_check = incoming.quantity;

    if (incoming.side == Side::BUY) {
      std::sort(asks.begin(), asks.end(), [](const Order &a, const Order &b) {
        if (a.price == b.price)
          return a.sequence_id < b.sequence_id;
        return a.price < b.price;
      });

      for (const auto &ask : asks) {
        if (ask.instrument_id != incoming.instrument_id)
          continue;
        if (incoming.type == OrderType::LIMIT && incoming.price < ask.price)
          break;
        if (qty_to_check == 0)
          break;
        if (ask.client_id == incoming.client_id)
          return true;
        qty_to_check = (qty_to_check > ask.quantity) ? (qty_to_check - ask.quantity) : 0;
      }
    } else {
      std::sort(bids.begin(), bids.end(), [](const Order &a, const Order &b) {
        if (a.price == b.price)
          return a.sequence_id < b.sequence_id;
        return a.price > b.price;
      });

      for (const auto &bid : bids) {
        if (bid.instrument_id != incoming.instrument_id)
          continue;
        if (incoming.type == OrderType::LIMIT && incoming.price > bid.price)
          break;
        if (qty_to_check == 0)
          break;
        if (bid.client_id == incoming.client_id)
          return true;
        qty_to_check = (qty_to_check > bid.quantity) ? (qty_to_check - bid.quantity) : 0;
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
    seen_sequences.reserve(2000000);
  }

  void Clear() override {
    bids.clear();
    asks.clear();
    seen_sequences.clear();
    bids.reserve(100000);
    asks.reserve(100000);
    seen_sequences.reserve(2000000);
  }

  void ProcessOrder(const Order &order) override {
    if (!seen_sequences.insert(order.sequence_id).second)
      return;

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
          it = asks.erase(it);
        } else if (current.quantity == 0) {
          break;
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
        } else if (current.quantity == 0) {
          break;
        }
      }

      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        asks.push_back(current);
      }
    }
  }

  std::vector<Order> GetRestingOrders(uint16_t instrument_id) const override {
    std::vector<Order> resting;
    for (const auto &b : bids)
      if (b.instrument_id == instrument_id)
        resting.push_back(b);
    for (const auto &a : asks)
      if (a.instrument_id == instrument_id)
        resting.push_back(a);
    return resting;
  }
};

ExchangeEngine *CreateEngine() { return new SlowEngine(); }
