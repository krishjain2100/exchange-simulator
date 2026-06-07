#include "Constants.h"
#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kNullNode = 0;
constexpr size_t kPoolCapacity = 4'000'000;

struct BookNode {
  Order order{};
  uint32_t prev = kNullNode;
  uint32_t next = kNullNode;
};

struct Level {
  uint32_t head = kNullNode;
  uint32_t tail = kNullNode;
};

struct OrderLoc {
  uint16_t inst;
  Side side;
  uint64_t price;
  uint32_t node;
};

class NodePool {
  std::vector<BookNode> nodes_;
  uint32_t free_head_ = kNullNode;

public:
  void Init() {
    nodes_.resize(kPoolCapacity + 1);
    free_head_ = 1;
    for (uint32_t i = 1; i < kPoolCapacity; ++i)
      nodes_[i].next = i + 1;
    nodes_[kPoolCapacity].next = kNullNode;
  }

  uint32_t Alloc(const Order &order) {
    if (free_head_ == kNullNode) [[unlikely]]
      return kNullNode;
    const uint32_t idx = free_head_;
    free_head_ = nodes_[idx].next;
    nodes_[idx].order = order;
    nodes_[idx].prev = kNullNode;
    nodes_[idx].next = kNullNode;
    return idx;
  }

  void Free(uint32_t idx) {
    nodes_[idx].prev = kNullNode;
    nodes_[idx].next = free_head_;
    free_head_ = idx;
  }

  BookNode &operator[](uint32_t idx) { return nodes_[idx]; }
  const BookNode &operator[](uint32_t idx) const { return nodes_[idx]; }
};

using BidLevels = std::map<uint64_t, Level, std::greater<uint64_t>>;
using AskLevels = std::map<uint64_t, Level, std::less<uint64_t>>;

struct InstrumentBook {
  BidLevels bids;
  AskLevels asks;
};

inline uint32_t MatchQty(uint32_t taker_qty, uint32_t maker_qty) {
  return taker_qty < maker_qty ? taker_qty : maker_qty;
}

void AppendToLevel(Level &lvl, uint32_t node, NodePool &pool) {
  if (lvl.head == kNullNode) {
    lvl.head = node;
    lvl.tail = node;
    return;
  }
  pool[lvl.tail].next = node;
  pool[node].prev = lvl.tail;
  lvl.tail = node;
}

void UnlinkFromLevel(Level &lvl, uint32_t node, NodePool &pool) {
  const uint32_t prev = pool[node].prev;
  const uint32_t next = pool[node].next;

  if (prev != kNullNode)
    pool[prev].next = next;
  else
    lvl.head = next;

  if (next != kNullNode)
    pool[next].prev = prev;
  else
    lvl.tail = prev;
}

} // namespace

class OptimizedEngine : public ExchangeEngine {
  NodePool pool_;
  std::array<InstrumentBook, SHARD_COUNT> books_;
  std::unordered_map<uint64_t, OrderLoc> order_index_;
  std::unordered_set<uint64_t> seen_sequences_;

  void HandleCancel(const Order &cancel_order) {
    const uint64_t target_seq = cancel_order.price;
    auto loc_it = order_index_.find(target_seq);
    if (loc_it == order_index_.end()) [[unlikely]]
      return;

    const OrderLoc loc = loc_it->second;
    order_index_.erase(loc_it);
    InstrumentBook &book = books_[loc.inst];

    if (loc.side == Side::BUY) {
      auto lvl_it = book.bids.find(loc.price);
      if (lvl_it != book.bids.end()) {
        UnlinkFromLevel(lvl_it->second, loc.node, pool_);
        if (lvl_it->second.head == kNullNode)
          book.bids.erase(lvl_it);
      }
    } else {
      auto lvl_it = book.asks.find(loc.price);
      if (lvl_it != book.asks.end()) {
        UnlinkFromLevel(lvl_it->second, loc.node, pool_);
        if (lvl_it->second.head == kNullNode)
          book.asks.erase(lvl_it);
      }
    }
    pool_.Free(loc.node);
  }

  bool WouldSelfMatch(const Order &incoming) const {
    const uint16_t inst = incoming.instrument_id;
    if (inst < 1 || inst > NUM_INSTRUMENTS) [[unlikely]]
      return false;

    uint32_t qty_left = incoming.quantity;
    const InstrumentBook &book = books_[inst];

    if (incoming.side == Side::BUY) {
      for (const auto &[price, lvl] : book.asks) {
        if (incoming.type == OrderType::LIMIT && incoming.price < price)
          break;
        if (qty_left == 0)
          break;
        for (uint32_t node = lvl.head; node != kNullNode;
             node = pool_[node].next) {
          const Order &maker = pool_[node].order;
          if (maker.client_id == incoming.client_id)
            return true;
          qty_left = qty_left > maker.quantity ? qty_left - maker.quantity : 0;
          if (qty_left == 0)
            break;
        }
      }
    } else {
      for (const auto &[price, lvl] : book.bids) {
        if (incoming.type == OrderType::LIMIT && incoming.price > price)
          break;
        if (qty_left == 0)
          break;
        for (uint32_t node = lvl.head; node != kNullNode;
             node = pool_[node].next) {
          const Order &maker = pool_[node].order;
          if (maker.client_id == incoming.client_id)
            return true;
          qty_left = qty_left > maker.quantity ? qty_left - maker.quantity : 0;
          if (qty_left == 0)
            break;
        }
      }
    }
    return false;
  }

  void MatchAgainstAsks(Order &taker) {
    const uint16_t inst = taker.instrument_id;
    auto &ask_book = books_[inst].asks;

    for (auto it = ask_book.begin(); it != ask_book.end();) {
      if (taker.quantity == 0)
        break;

      const uint64_t ask_price = it->first;
      if (taker.type == OrderType::LIMIT && taker.price < ask_price)
        break;

      uint32_t node = it->second.head;
      while (node != kNullNode && taker.quantity > 0) {
        const uint32_t next = pool_[node].next;
        Order &maker = pool_[node].order;
        const uint32_t trade_qty = MatchQty(taker.quantity, maker.quantity);

        Telemetry::ReportTrade(inst, maker.sequence_id, taker.sequence_id,
                               trade_qty, ask_price);

        taker.quantity -= trade_qty;
        maker.quantity -= trade_qty;

        if (maker.quantity == 0) {
          order_index_.erase(maker.sequence_id);
          UnlinkFromLevel(it->second, node, pool_);
          pool_.Free(node);
        }
        node = next;
      }

      if (it->second.head == kNullNode)
        it = ask_book.erase(it);
      else
        ++it;
    }
  }

  void MatchAgainstBids(Order &taker) {
    const uint16_t inst = taker.instrument_id;
    auto &bid_book = books_[inst].bids;

    for (auto it = bid_book.begin(); it != bid_book.end();) {
      if (taker.quantity == 0)
        break;

      const uint64_t bid_price = it->first;
      if (taker.type == OrderType::LIMIT && taker.price > bid_price)
        break;

      uint32_t node = it->second.head;
      while (node != kNullNode && taker.quantity > 0) {
        const uint32_t next = pool_[node].next;
        Order &maker = pool_[node].order;
        const uint32_t trade_qty = MatchQty(taker.quantity, maker.quantity);

        Telemetry::ReportTrade(inst, maker.sequence_id, taker.sequence_id,
                               trade_qty, bid_price);

        taker.quantity -= trade_qty;
        maker.quantity -= trade_qty;

        if (maker.quantity == 0) {
          order_index_.erase(maker.sequence_id);
          UnlinkFromLevel(it->second, node, pool_);
          pool_.Free(node);
        }
        node = next;
      }

      if (it->second.head == kNullNode)
        it = bid_book.erase(it);
      else
        ++it;
    }
  }

  void RestLimitBuy(Order &order) {
    const uint16_t inst = order.instrument_id;
    const uint32_t node = pool_.Alloc(order);
    if (node == kNullNode) [[unlikely]]
      return;

    Level &lvl = books_[inst].bids[order.price];
    AppendToLevel(lvl, node, pool_);
    order_index_[order.sequence_id] = {inst, Side::BUY, order.price, node};
  }

  void RestLimitSell(Order &order) {
    const uint16_t inst = order.instrument_id;
    const uint32_t node = pool_.Alloc(order);
    if (node == kNullNode) [[unlikely]]
      return;

    Level &lvl = books_[inst].asks[order.price];
    AppendToLevel(lvl, node, pool_);
    order_index_[order.sequence_id] = {inst, Side::SELL, order.price, node};
  }

public:
  void Init() override {
    std::cout << "[OptimizedEngine] Booting Level 4 engine (pooled intrusive "
                 "books)..."
              << std::endl;
    pool_.Init();
    order_index_.clear();
    order_index_.reserve(2'000'000);
    seen_sequences_.clear();
    seen_sequences_.reserve(2'000'000);
  }

  void ProcessOrder(const Order &order) override {
    const uint16_t inst = order.instrument_id;
    if (inst < 1 || inst > NUM_INSTRUMENTS) [[unlikely]]
      return;

    if (!seen_sequences_.insert(order.sequence_id).second) [[unlikely]]
      return;

    if (order.type == OrderType::CANCEL) [[unlikely]] {
      HandleCancel(order);
      return;
    }

    if (WouldSelfMatch(order)) [[unlikely]]
      return;

    Order taker = order;
    if (taker.side == Side::BUY) {
      MatchAgainstAsks(taker);
      if (taker.quantity > 0 && taker.type == OrderType::LIMIT)
        RestLimitBuy(taker);
    } else {
      MatchAgainstBids(taker);
      if (taker.quantity > 0 && taker.type == OrderType::LIMIT)
        RestLimitSell(taker);
    }
  }

  std::vector<Order> GetRestingOrders(uint16_t instrument_id) const override {
    std::vector<Order> resting;
    if (instrument_id < 1 || instrument_id > NUM_INSTRUMENTS)
      return resting;

    const InstrumentBook &book = books_[instrument_id];
    for (const auto &[price, lvl] : book.bids) {
      (void)price;
      for (uint32_t node = lvl.head; node != kNullNode;
           node = pool_[node].next)
        resting.push_back(pool_[node].order);
    }
    for (const auto &[price, lvl] : book.asks) {
      (void)price;
      for (uint32_t node = lvl.head; node != kNullNode;
           node = pool_[node].next)
        resting.push_back(pool_[node].order);
    }
    return resting;
  }
};

ExchangeEngine *CreateEngine() { return new OptimizedEngine(); }
