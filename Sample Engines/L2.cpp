#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <array>
#include <iostream>
#include <list>
#include <map>
#include <unordered_set>

class IntermediateEngine : public ExchangeEngine {
private:
  static constexpr int INSTRUMENT_COUNT = 5;

  // UPGRADE: Use std::list to prevent O(N) memory shifts during erasures
  using DescBook = std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>;
  using AscBook = std::map<uint64_t, std::list<Order>, std::less<uint64_t>>;

  std::array<DescBook, INSTRUMENT_COUNT> bids;
  std::array<AscBook, INSTRUMENT_COUNT> asks;
  std::unordered_set<uint64_t> seen_sequences;

  void HandleCancel(const Order &cancel_order) {
    uint16_t inst = cancel_order.instrument_id;
    uint64_t target_seq = cancel_order.price;

    auto erase_from = [&](auto &book) -> bool {
      for (auto map_it = book.begin(); map_it != book.end(); ++map_it) {
        auto &level = map_it->second;
        for (auto list_it = level.begin(); list_it != level.end(); ++list_it) {
          if (list_it->sequence_id == target_seq) {
            level.erase(list_it);
            if (level.empty())
              book.erase(map_it);
            return true;
          }
        }
      }
      return false;
    };

    if (erase_from(bids[inst]) || erase_from(asks[inst]))
      return;
  }

  bool WouldSelfMatch(const Order &incoming) {
    uint16_t inst = incoming.instrument_id;
    uint32_t qty_to_check = incoming.quantity;

    if (incoming.side == Side::BUY) {
      for (const auto &[price, level] : asks[inst]) {
        if (incoming.type == OrderType::LIMIT && incoming.price < price)
          break;
        if (qty_to_check == 0)
          break;
        for (const auto &ask : level) {
          if (ask.client_id == incoming.client_id)
            return true;
          uint32_t r = ask.quantity;
          qty_to_check = (qty_to_check > r) ? (qty_to_check - r) : 0;
          if (qty_to_check == 0)
            break;
        }
      }
    } else {
      for (const auto &[price, level] : bids[inst]) {
        if (incoming.type == OrderType::LIMIT && incoming.price > price)
          break;
        if (qty_to_check == 0)
          break;
        for (const auto &bid : level) {
          if (bid.client_id == incoming.client_id)
            return true;
          uint32_t r = bid.quantity;
          qty_to_check = (qty_to_check > r) ? (qty_to_check - r) : 0;
          if (qty_to_check == 0)
            break;
        }
      }
    }
    return false;
  }

public:
  void Init() override {
    std::cout << "[IntermediateEngine] Booting mid-level engine (Red-Black "
                 "Trees + Lists)..."
              << std::endl;
    seen_sequences.reserve(2000000);
  }

  void Clear() override {
    for (auto &book : bids)
      book.clear();
    for (auto &book : asks)
      book.clear();
    seen_sequences.clear();
    seen_sequences.reserve(2000000);
  }

  void ProcessOrder(const Order &current) override {
    if (!seen_sequences.insert(current.sequence_id).second)
      return;

    if (current.type == OrderType::CANCEL) {
      HandleCancel(current);
      return;
    }

    if (WouldSelfMatch(current))
      return;

    uint16_t inst = current.instrument_id;
    Order order = current;

    if (order.side == Side::BUY) {
      auto it = asks[inst].begin();

      while (it != asks[inst].end() && order.quantity > 0) {
        uint64_t ask_price = it->first;
        if (order.type == OrderType::LIMIT && order.price < ask_price)
          break;

        auto &level = it->second;
        auto order_it = level.begin();

        while (order_it != level.end() && order.quantity > 0) {
          uint32_t match_qty = std::min(order.quantity, order_it->quantity);
          Telemetry::ReportTrade(inst, order_it->sequence_id, order.sequence_id,
                                 match_qty, ask_price);

          order.quantity -= match_qty;
          order_it->quantity -= match_qty;

          if (order_it->quantity == 0) {
            order_it = level.erase(order_it);
          } else if (order.quantity == 0) {
            break;
          }
        }

        if (level.empty()) {
          it = asks[inst].erase(it);
        } else {
          ++it;
        }
      }

      if (order.quantity > 0 && order.type == OrderType::LIMIT) {
        bids[inst][order.price].push_back(order);
      }

    } else {
      auto it = bids[inst].begin();

      while (it != bids[inst].end() && order.quantity > 0) {
        uint64_t bid_price = it->first;
        if (order.type == OrderType::LIMIT && order.price > bid_price)
          break;

        auto &level = it->second;
        auto order_it = level.begin();

        while (order_it != level.end() && order.quantity > 0) {
          uint32_t match_qty = std::min(order.quantity, order_it->quantity);
          Telemetry::ReportTrade(inst, order_it->sequence_id, order.sequence_id,
                                 match_qty, bid_price);

          order.quantity -= match_qty;
          order_it->quantity -= match_qty;

          if (order_it->quantity == 0) {
            order_it = level.erase(order_it);
          } else if (order.quantity == 0) {
            break;
          }
        }

        if (level.empty()) {
          it = bids[inst].erase(it);
        } else {
          ++it;
        }
      }

      if (order.quantity > 0 && order.type == OrderType::LIMIT) {
        asks[inst][order.price].push_back(order);
      }
    }
  }

  std::vector<Order> GetRestingOrders(uint16_t instrument_id) const override {
    std::vector<Order> resting;
    if (instrument_id >= INSTRUMENT_COUNT) return resting;
    for (const auto &[price, level] : bids[instrument_id]) {
      for (const auto &o : level)
        resting.push_back(o);
    }
    for (const auto &[price, level] : asks[instrument_id]) {
      for (const auto &o : level)
        resting.push_back(o);
    }
    return resting;
  }
};

ExchangeEngine *CreateEngine() { return new IntermediateEngine(); }
