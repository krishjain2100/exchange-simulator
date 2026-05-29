#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct Order {
  uint64_t seq_id;
  uint32_t client_id;
  uint16_t inst_id;
  int type;
  int side;
  uint64_t price;
  uint32_t qty;

  bool operator==(const Order &other) const {
    return seq_id == other.seq_id && client_id == other.client_id &&
           inst_id == other.inst_id && type == other.type &&
           side == other.side && price == other.price && qty == other.qty;
  }
};

struct Trade {
  uint16_t inst_id;
  uint64_t maker_id;
  uint64_t taker_id;
  uint32_t qty;
  uint64_t price;

  bool operator==(const Trade &other) const {
    return inst_id == other.inst_id && maker_id == other.maker_id &&
           taker_id == other.taker_id && qty == other.qty &&
           price == other.price;
  }
};

class GoldenVerifier {
private:
  static constexpr int INSTRUMENT_COUNT = 10;
  using DescBook = std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>;
  using AscBook = std::map<uint64_t, std::list<Order>, std::less<uint64_t>>;

  std::array<DescBook, INSTRUMENT_COUNT> bids;
  std::array<AscBook, INSTRUMENT_COUNT> asks;

  struct OrderLocation {
    uint64_t price;
    int side;
    std::list<Order>::iterator list_iterator;
  };
  std::unordered_map<uint64_t, OrderLocation> order_index;

  void HandleCancel(uint64_t target_id, uint16_t inst) {
    auto loc_it = order_index.find(target_id);
    if (loc_it == order_index.end())
      return;

    uint64_t price = loc_it->second.price;
    int side = loc_it->second.side;
    auto list_it = loc_it->second.list_iterator;

    if (side == 1) { // BUY
      bids[inst][price].erase(list_it);
      if (bids[inst][price].empty())
        bids[inst].erase(price);
    } else { // SELL
      asks[inst][price].erase(list_it);
      if (asks[inst][price].empty())
        asks[inst].erase(price);
    }
    order_index.erase(loc_it);
  }

  bool WouldSelfMatch(const Order &incoming) {
    uint16_t inst = incoming.inst_id;
    uint32_t client = incoming.client_id;
    uint32_t qty_to_check = incoming.qty;

    if (incoming.side == 1) { // BUY
      for (const auto &pm : asks[inst]) {
        const uint64_t ask_price = pm.first;
        if (incoming.type == 1 && incoming.price < ask_price)
          break;
        if (qty_to_check == 0)
          break;
        for (const auto &resting : pm.second) {
          if (resting.client_id == client)
            return true;
          const uint32_t r = resting.qty;
          qty_to_check = (qty_to_check > r) ? (qty_to_check - r) : 0;
          if (qty_to_check == 0)
            break;
        }
      }
    } else { // SELL
      for (const auto &pm : bids[inst]) {
        const uint64_t bid_price = pm.first;
        if (incoming.type == 1 && incoming.price > bid_price)
          break;
        if (qty_to_check == 0)
          break;
        for (const auto &resting : pm.second) {
          if (resting.client_id == client)
            return true;
          const uint32_t r = resting.qty;
          qty_to_check = (qty_to_check > r) ? (qty_to_check - r) : 0;
          if (qty_to_check == 0)
            break;
        }
      }
    }
    return false;
  }

public:
  std::vector<Trade> trades;

  GoldenVerifier() { order_index.reserve(2000000); }

  void ProcessOrder(Order current) {
    uint16_t inst = current.inst_id;

    if (current.type == 3) {
      HandleCancel(current.price, inst);
      return;
    }

    if (WouldSelfMatch(current))
      return;

    if (current.side == 1) { // BUY
      auto &ask_book = asks[inst];
      auto it = ask_book.begin();

      while (it != ask_book.end() && current.qty > 0) {
        uint64_t ask_price = it->first;
        if (current.type == 1 && current.price < ask_price)
          break;

        auto &orders_at_price = it->second;
        auto order_it = orders_at_price.begin();

        while (order_it != orders_at_price.end() && current.qty > 0) {
          Order &maker = *order_it;
          uint32_t match_qty = std::min(current.qty, maker.qty);

          trades.push_back(
              {inst, maker.seq_id, current.seq_id, match_qty, ask_price});

          current.qty -= match_qty;
          maker.qty -= match_qty;

          if (maker.qty == 0) {
            order_index.erase(maker.seq_id);
            order_it = orders_at_price.erase(order_it);
          } else {
            ++order_it;
          }
        }
        if (orders_at_price.empty())
          it = ask_book.erase(it);
        else
          ++it;
      }
      if (current.qty > 0 && current.type == 1) {
        auto &lst = bids[inst][current.price];
        lst.emplace_back(current);
        auto new_it = std::prev(lst.end());
        order_index[new_it->seq_id] = {new_it->price, 1, new_it};
      }
    } else { // SELL
      auto &bid_book = bids[inst];
      auto it = bid_book.begin();

      while (it != bid_book.end() && current.qty > 0) {
        uint64_t bid_price = it->first;
        if (current.type == 1 && current.price > bid_price)
          break;

        auto &orders_at_price = it->second;
        auto order_it = orders_at_price.begin();

        while (order_it != orders_at_price.end() && current.qty > 0) {
          Order &maker = *order_it;
          uint32_t match_qty = std::min(current.qty, maker.qty);

          trades.push_back(
              {inst, maker.seq_id, current.seq_id, match_qty, bid_price});

          current.qty -= match_qty;
          maker.qty -= match_qty;

          if (maker.qty == 0) {
            order_index.erase(maker.seq_id);
            order_it = orders_at_price.erase(order_it);
          } else {
            ++order_it;
          }
        }
        if (orders_at_price.empty())
          it = bid_book.erase(it);
        else
          ++it;
      }
      if (current.qty > 0 && current.type == 1) {
        auto &lst = asks[inst][current.price];
        lst.emplace_back(current);
        auto new_it = std::prev(lst.end());
        order_index[new_it->seq_id] = {new_it->price, 2, new_it};
      }
    }
  }
  std::vector<Order> GetRestingOrders() const {
    std::vector<Order> resting;
    for (int i = 0; i < INSTRUMENT_COUNT; ++i) {
      for (const auto &[price, level] : bids[i]) {
        for (const auto &o : level)
          resting.push_back(o);
      }
      for (const auto &[price, level] : asks[i]) {
        for (const auto &o : level)
          resting.push_back(o);
      }
    }
    return resting;
  }
};

// Bypasses MacOS sscanf formatting bugs by advancing raw memory pointers
std::vector<Order> ParseInputLedger(const std::string &path) {
  std::vector<Order> orders;
  orders.reserve(10000000);
  FILE *file = fopen(path.c_str(), "r");
  if (!file)
    return orders;

  char buffer[256];
  if (fgets(buffer, sizeof(buffer), file)) {
  } // Skip header

  while (fgets(buffer, sizeof(buffer), file)) {
    char *ptr = buffer;
    Order o;
    o.seq_id = strtoull(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    o.client_id = strtoul(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    o.inst_id = strtoul(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    o.type = strtoul(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    o.side = strtoul(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    o.price = strtoull(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    o.qty = strtoul(ptr, &ptr, 10);
    orders.push_back(o);
  }
  fclose(file);
  return orders;
}

std::vector<Trade> ParseTradeLedger(const std::string &path) {
  std::vector<Trade> trades;
  trades.reserve(10000000);
  FILE *file = fopen(path.c_str(), "r");
  if (!file)
    return trades;

  char buffer[256];
  if (fgets(buffer, sizeof(buffer), file)) {
  } // Skip header

  while (fgets(buffer, sizeof(buffer), file)) {
    char *ptr = buffer;
    Trade t;
    t.inst_id = strtoul(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    t.maker_id = strtoull(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    t.taker_id = strtoull(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    t.qty = strtoul(ptr, &ptr, 10);
    if (*ptr == ',')
      ++ptr;
    t.price = strtoull(ptr, &ptr, 10);
    trades.push_back(t);
  }
  fclose(file);
  return trades;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: ./verifier /path/to/job_dir\n";
    return 1;
  }

  std::string dir = argv[1];
  std::cout << "[Verifier] Loading ledgers from " << dir << "...\n";

  auto inputs = ParseInputLedger(dir + "/input_ledger.csv");
  auto cpp_trades = ParseTradeLedger(dir + "/trade_ledger.csv");
  auto cpp_book = ParseInputLedger(dir + "/final_book.csv");

  std::cout << "[Verifier] Processing " << inputs.size()
            << " orders through Golden Model...\n";

  GoldenVerifier golden;
  for (const auto &o : inputs) {
    golden.ProcessOrder(o);
  }

  std::cout << "[Verifier] Checking Trade Integrity...\n";
  if (golden.trades.size() != cpp_trades.size()) {
    std::cerr << "  FAILED — trade count: C++ Engine produced "
              << cpp_trades.size() << ", Golden Model expects "
              << golden.trades.size() << "\n";
    return 1;
  }

  for (size_t i = 0; i < cpp_trades.size(); ++i) {
    if (!(cpp_trades[i] == golden.trades[i])) {
      std::cerr << "  FAILED — trade mismatch at index " << i << "\n";
      return 1;
    }
  }

  std::cout << "[Verifier] Checking Final Book State Integrity...\n";
  auto golden_book = golden.GetRestingOrders();

  if (cpp_book.size() != golden_book.size()) {
    std::cerr << "  FAILED — resting order count: C++ Engine left "
              << cpp_book.size() << " orders, Golden Model expects "
              << golden_book.size() << "\n";
    return 1;
  }

  auto sort_by_seq = [](const Order &a, const Order &b) {
    return a.seq_id < b.seq_id;
  };

  std::sort(cpp_book.begin(), cpp_book.end(), sort_by_seq);
  std::sort(golden_book.begin(), golden_book.end(), sort_by_seq);

  for (size_t i = 0; i < cpp_book.size(); ++i) {
    if (!(cpp_book[i] == golden_book[i])) {
      std::cerr << "  FAILED — final book mismatch at sequence ID "
                << golden_book[i].seq_id << "\n";
      return 1;
    }
  }

  std::cout << "SUCCESS — C++ engine matches the Golden Model exactly.\n";
  return 0;
}