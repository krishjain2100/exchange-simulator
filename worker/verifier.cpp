#include "Constants.h"
#include "Hash.h"
#include "Order.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Trade {
  uint16_t inst_id;
  uint64_t maker_id;
  uint64_t taker_id;
  uint32_t qty;
  uint64_t price;
};

class GoldenVerifier {
private:
  using DescBook = std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>;
  using AscBook = std::map<uint64_t, std::list<Order>, std::less<uint64_t>>;

  // Sharded Internal State
  std::array<DescBook, NUM_INSTRUMENTS + 1> bids;
  std::array<AscBook, NUM_INSTRUMENTS + 1> asks;

  struct OrderLocation {
    uint16_t inst;
    uint64_t price;
    Side side;
    std::list<Order>::iterator list_iterator;
  };
  std::unordered_map<uint64_t, OrderLocation> order_index;
  std::unordered_set<uint64_t> seen_sequences;

  void HandleCancel(uint64_t target_id, uint16_t /*cancel_inst*/) {
    auto loc_it = order_index.find(target_id);
    if (loc_it == order_index.end())
      return;

    const uint16_t inst = loc_it->second.inst;
    uint64_t price = loc_it->second.price;
    Side side = loc_it->second.side;
    auto list_it = loc_it->second.list_iterator;

    if (side == Side::BUY) {
      bids[inst][price].erase(list_it);
      if (bids[inst][price].empty())
        bids[inst].erase(price);
    } else {
      asks[inst][price].erase(list_it);
      if (asks[inst][price].empty())
        asks[inst].erase(price);
    }
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
            return true;
          qty_to_check = (qty_to_check > resting.quantity)
                             ? (qty_to_check - resting.quantity)
                             : 0;
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
            return true;
          qty_to_check = (qty_to_check > resting.quantity)
                             ? (qty_to_check - resting.quantity)
                             : 0;
          if (qty_to_check == 0)
            break;
        }
      }
    }
    return false;
  }

public:
  // Sharded Trade Ledgers
  std::vector<Trade> trades[NUM_INSTRUMENTS + 1];

  GoldenVerifier() {
    order_index.reserve(2000000);
    seen_sequences.reserve(2000000);
  }

  void ProcessOrder(Order current) {
    uint16_t inst = current.instrument_id;
    if (inst < 1 || inst > NUM_INSTRUMENTS)
      return;

    if (!seen_sequences.insert(current.sequence_id).second)
      return;

    if (current.type == OrderType::CANCEL) {
      HandleCancel(current.price, inst);
      return;
    }

    if (WouldSelfMatch(current))
      return;

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

          // Push to the specific instrument's ledger
          trades[inst].push_back({inst, maker.sequence_id, current.sequence_id,
                                  match_qty, ask_price});

          current.quantity -= match_qty;
          maker.quantity -= match_qty;

          if (maker.quantity == 0) {
            order_index.erase(maker.sequence_id);
            order_it = orders_at_price.erase(order_it);
          } else if (current.quantity == 0) {
            break;
          }
        }
        if (orders_at_price.empty())
          it = ask_book.erase(it);
        else
          ++it;
      }
      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        auto &lst = bids[inst][current.price];
        lst.emplace_back(current);
        auto new_it = std::prev(lst.end());
        order_index[new_it->sequence_id] = {inst, new_it->price, Side::BUY,
                                            new_it};
      }
    } else {
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

          // Push to the specific instrument's ledger
          trades[inst].push_back({inst, maker.sequence_id, current.sequence_id,
                                  match_qty, bid_price});

          current.quantity -= match_qty;
          maker.quantity -= match_qty;

          if (maker.quantity == 0) {
            order_index.erase(maker.sequence_id);
            order_it = orders_at_price.erase(order_it);
          } else if (current.quantity == 0) {
            break;
          }
        }
        if (orders_at_price.empty())
          it = bid_book.erase(it);
        else
          ++it;
      }
      if (current.quantity > 0 && current.type == OrderType::LIMIT) {
        auto &lst = asks[inst][current.price];
        lst.emplace_back(current);
        auto new_it = std::prev(lst.end());
        order_index[new_it->sequence_id] = {inst, new_it->price, Side::SELL,
                                            new_it};
      }
    }
  }

  // SYNCED API: The Oracle now pulls resting orders strictly by Shard
  std::vector<Order> GetRestingOrders(uint16_t inst) const {
    std::vector<Order> resting;
    for (const auto &[price, level] : bids[inst]) {
      for (const auto &o : level)
        resting.push_back(o);
    }
    for (const auto &[price, level] : asks[inst]) {
      for (const auto &o : level)
        resting.push_back(o);
    }
    return resting;
  }
};

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: ./verifier /path/to/job_dir\n";
    return 1;
  }

  std::string dir = argv[1];
  std::cout << "[Verifier] Loading BINARY ledgers from " << dir << "...\n";

  // 1. Load Global Input Ledger
  std::vector<Order> inputs;
  FILE *f_in = fopen((dir + "/input_ledger.bin").c_str(), "rb");
  if (f_in) {
    fseek(f_in, 0, SEEK_END);
    size_t count = ftell(f_in) / sizeof(Order);
    rewind(f_in);
    inputs.resize(count);
    fread(inputs.data(), sizeof(Order), count, f_in);
    fclose(f_in);
  } else {
    std::cerr << "[Verifier] ERROR: Could not open input_ledger.bin\n";
    return 1;
  }

  // 2. Load Sharded C++ Engine Payload
  std::vector<uint64_t> cpp_payload[NUM_INSTRUMENTS + 1];
  FILE *f_payload = fopen((dir + "/payload.bin").c_str(), "rb");
  if (f_payload) {
    for (int i = 1; i <= NUM_INSTRUMENTS; ++i) {
      uint64_t chunk_size = 0;
      if (fread(&chunk_size, sizeof(uint64_t), 1, f_payload) != 1)
        break;
      cpp_payload[i].resize(chunk_size);
      if (chunk_size > 0) {
        fread(cpp_payload[i].data(), sizeof(uint64_t), chunk_size, f_payload);
      }
    }
    fclose(f_payload);
  } else {
    std::cerr << "[Verifier] ERROR: Could not open payload.bin\n";
    return 1;
  }

  std::cout << "[Verifier] Processing " << inputs.size()
            << " orders through Golden Model...\n";

  GoldenVerifier golden;
  for (const auto &o : inputs) {
    golden.ProcessOrder(o);
  }

  // 3. Verify Correctness Per-Instrument
  for (int i = 1; i <= NUM_INSTRUMENTS; ++i) {
    std::cout << "[Verifier] Validating Instrument " << i << "...\n";

    if (cpp_payload[i].empty()) {
      std::cerr
          << "  FAILED — Engine produced no output payload for Instrument " << i
          << ".\n";
      return 1;
    }

    // Extract the Final Book Hash for this specific instrument
    uint64_t cpp_book_hash = cpp_payload[i].back();
    cpp_payload[i].pop_back(); // Remove it so only trade hashes remain

    // --- CHECK 1: TRADE HASH TRAIL ---
    if (golden.trades[i].size() != cpp_payload[i].size()) {
      std::cerr << "  FAILED — Trade count mismatch on Instrument " << i
                << ": C++ Engine produced " << cpp_payload[i].size()
                << " trades, Golden Model expects " << golden.trades[i].size()
                << "\n";
      return 1;
    }

    for (size_t j = 0; j < golden.trades[i].size(); ++j) {
      const auto &t = golden.trades[i][j];
      uint64_t expected_hash =
          HashTrade(t.inst_id, t.maker_id, t.taker_id, t.qty, t.price);

      if (expected_hash != cpp_payload[i][j]) {
        std::cerr << "\n  FAILED — Trade Hash Collision at Index " << j
                  << " on Instrument " << i << "!\n"
                  << "  The Golden Model generated:\n"
                  << "  Maker: " << t.maker_id << " | Taker: " << t.taker_id
                  << " | Qty: " << t.qty << " | Price: " << t.price << "\n"
                  << "  Your engine crossed the wrong orders.\n";
        return 1;
      }
    }

    // --- CHECK 2: FINAL BOOK HASH ---
    // We now query the Oracle's resting orders exactly how the Wrapper queried
    // the participant
    std::vector<Order> golden_resting = golden.GetRestingOrders(i);
    uint64_t expected_book_hash = HashSortedBook(golden_resting);

    if (expected_book_hash != cpp_book_hash) {
      std::cerr << "  FAILED — Final Book Hash Mismatch on Instrument " << i
                << ".\n"
                << "  Engine Hash: " << cpp_book_hash << "\n"
                << "  Golden Hash: " << expected_book_hash << "\n"
                << "  Your engine has a memory leak, a ghost level, or dropped "
                   "an order.\n";
      return 1;
    }
  }

  std::cout << "\nSUCCESS — C++ Engine is Cryptographically Verified Across "
               "All Shards!\n";
  return 0;
}
