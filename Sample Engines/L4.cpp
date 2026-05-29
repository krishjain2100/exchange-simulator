#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

class MockEngine : public ExchangeEngine {
private:
  static constexpr int INSTRUMENT_COUNT = 5;
  using DescBook = std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>;
  using AscBook = std::map<uint64_t, std::list<Order>, std::less<uint64_t>>;

  std::array<DescBook, INSTRUMENT_COUNT> bids;
  std::array<AscBook, INSTRUMENT_COUNT> asks;

  struct OrderLocation {
    std::list<Order> *price_level_list;
    std::list<Order>::iterator list_iterator;
  };

  std::unordered_map<uint64_t, OrderLocation> order_index;

  void HandleCancel(const Order &cancel_order) {
    auto loc_it = order_index.find(cancel_order.price);
    if (loc_it == order_index.end()) [[unlikely]]
      return;

    loc_it->second.price_level_list->erase(loc_it->second.list_iterator);

    order_index.erase(loc_it);
  }

  bool WouldSelfMatch(const Order &incoming) {
    uint16_t inst = incoming.instrument_id;
    uint32_t client = incoming.client_id;
    uint32_t qty_to_check = incoming.quantity;

    if (incoming.side == Side::BUY) {
      for (const auto &pm : asks[inst]) {
        if (incoming.type == OrderType::LIMIT && incoming.price < pm.first)
          break;
        if (qty_to_check == 0)
          break;

        for (const auto &resting : pm.second) {
          if (resting.client_id == client)
            return true; // Wash Trade Detected

          uint32_t r = resting.quantity;
          qty_to_check = (qty_to_check > r) ? (qty_to_check - r) : 0;
          if (qty_to_check == 0)
            break;
        }
      }
    } else {
      for (const auto &pm : bids[inst]) {
        if (incoming.type == OrderType::LIMIT && incoming.price > pm.first)
          break;
        if (qty_to_check == 0)
          break;

        for (const auto &resting : pm.second) {
          if (resting.client_id == client)
            return true; // Wash Trade Detected

          uint32_t r = resting.quantity;
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
    std::cout << "[MockEngine] Initializing structures..." << std::endl;
    for (int i = 0; i < INSTRUMENT_COUNT; ++i) {
      bids[i].clear();
      asks[i].clear();
    }
    order_index.clear();
    order_index.reserve(2000000);
  }

  void ProcessOrder(const Order &order) override {
    if (order.type == OrderType::CANCEL) [[unlikely]] {
      HandleCancel(order);
      return;
    }

    if (WouldSelfMatch(order)) [[unlikely]]
      return;

    Order current = order;
    uint16_t inst = current.instrument_id;

    if (current.side == Side::BUY) {
      auto &ask_book = asks[inst];
      auto it = ask_book.begin();

      while (it != ask_book.end() && current.quantity > 0) {
        uint64_t ask_price = it->first;
        if (current.type == OrderType::LIMIT && current.price < ask_price)
          break;

        auto &orders_at_price = it->second;
        auto order_it = orders_at_price.begin();

        while (order_it != orders_at_price.end() && current.quantity > 0) {
          Order &maker = *order_it;
          uint32_t match_qty = std::min(current.quantity, maker.quantity);

          Telemetry::ReportTrade(inst, maker.sequence_id, current.sequence_id,
                                 match_qty, ask_price);

          current.quantity -= match_qty;
          maker.quantity -= match_qty;

          if (maker.quantity == 0) {
            order_index.erase(maker.sequence_id);
            order_it = orders_at_price.erase(order_it);
          } else {
            ++order_it;
          }
        }

        if (orders_at_price.empty()) {
          it = ask_book.erase(it);
        } else {
          ++it;
        }
      }

      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        auto &lst = bids[inst][current.price];
        lst.emplace_back(std::move(current));
        auto new_it = std::prev(lst.end());

        order_index[new_it->sequence_id] = {&lst, new_it};
      }

    } else if (current.side == Side::SELL) {
      auto &bid_book = bids[inst];
      auto it = bid_book.begin();

      while (it != bid_book.end() && current.quantity > 0) {
        uint64_t bid_price = it->first;
        if (current.type == OrderType::LIMIT && current.price > bid_price)
          break;

        auto &orders_at_price = it->second;
        auto order_it = orders_at_price.begin();

        while (order_it != orders_at_price.end() && current.quantity > 0) {
          Order &maker = *order_it;
          uint32_t match_qty = std::min(current.quantity, maker.quantity);

          Telemetry::ReportTrade(inst, maker.sequence_id, current.sequence_id,
                                 match_qty, bid_price);

          current.quantity -= match_qty;
          maker.quantity -= match_qty;

          if (maker.quantity == 0) {
            order_index.erase(maker.sequence_id);
            order_it = orders_at_price.erase(order_it);
          } else {
            ++order_it;
          }
        }

        if (orders_at_price.empty()) {
          it = bid_book.erase(it);
        } else {
          ++it;
        }
      }

      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        auto &lst = asks[inst][current.price];
        lst.emplace_back(std::move(current));
        auto new_it = std::prev(lst.end());

        order_index[new_it->sequence_id] = {&lst, new_it};
      }
    }
  }

  std::vector<Order> GetRestingOrders() const override {
    std::vector<Order> resting;
    resting.reserve(order_index.size());
    for (int i = 0; i < INSTRUMENT_COUNT; ++i) {
      for (const auto &pm : bids[i]) {
        for (const auto &order : pm.second)
          resting.push_back(order);
      }
      for (const auto &pm : asks[i]) {
        for (const auto &order : pm.second)
          resting.push_back(order);
      }
    }
    return resting;
  }
};

ExchangeEngine *CreateEngine() { return new MockEngine(); }