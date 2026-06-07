// Generates the canonical benchmark_load.bin used in Phase 2 scoring.
// Output: raw packed Order frames (36 bytes each), deterministic across runs.
// Simulates realistic multi-instrument market flow (limits, cancels, crosses)
// without synthetic edge-case injections.

#include "Order.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kNumInstruments = 4;
constexpr int kNumClients = 40;
constexpr int kBasePrice = 10000;
constexpr int kBookDepth = 10;
constexpr int kSpreadTick = 2;
constexpr size_t kMaxResting = 120'000;
constexpr size_t kTargetOrders = 5'000'000;

struct CancelableOrder {
  uint64_t seq;
  uint32_t client;
  uint16_t instrument;
  Side side;
};

Order MakeOrder(uint64_t seq, uint32_t client, uint16_t instr, OrderType type,
                uint64_t price, uint32_t qty, Side side) {
  Order order{};
  order.sequence_id = seq;
  order.timestamp_ns = 0;
  order.client_id = client;
  order.instrument_id = instr;
  order.type = type;
  order.price = price;
  order.quantity = qty;
  order.side = side;
  return order;
}

// Wire format: price holds target sequence_id, quantity must be 0.
Order MakeCancelOrder(uint64_t seq, uint32_t client, uint16_t instr,
                      uint64_t target_seq, Side side) {
  Order order{};
  order.sequence_id = seq;
  order.timestamp_ns = 0;
  order.client_id = client;
  order.instrument_id = instr;
  order.type = OrderType::CANCEL;
  order.price = target_seq;
  order.quantity = 0;
  order.side = side;
  return order;
}

uint64_t MidFor(uint16_t instr) {
  return static_cast<uint64_t>(kBasePrice) * instr;
}

void TrackResting(std::deque<CancelableOrder> &resting, CancelableOrder entry) {
  resting.push_back(entry);
  while (resting.size() > kMaxResting)
    resting.pop_front();
}

void SeedBook(std::vector<Order> &out, uint64_t &seq,
              std::deque<CancelableOrder> &resting) {
  for (uint16_t instr = 1; instr <= kNumInstruments; ++instr) {
    const uint64_t mid = MidFor(instr);
    for (int level = 1; level <= kBookDepth; ++level) {
      const uint32_t client =
          static_cast<uint32_t>((instr * 10 + level) % kNumClients + 1);
      const uint32_t qty = static_cast<uint32_t>(10 + level * 3);

      const uint64_t bid_seq = seq++;
      out.push_back(MakeOrder(bid_seq, client, instr, OrderType::LIMIT,
                              mid - static_cast<uint64_t>(level * kSpreadTick),
                              qty, Side::BUY));
      TrackResting(resting, {bid_seq, client, instr, Side::BUY});

      const uint64_t ask_seq = seq++;
      const uint32_t ask_client = client + 100;
      out.push_back(MakeOrder(ask_seq, ask_client, instr, OrderType::LIMIT,
                              mid + static_cast<uint64_t>(level * kSpreadTick),
                              qty, Side::SELL));
      TrackResting(resting, {ask_seq, ask_client, instr, Side::SELL});
    }
  }
}

bool CancelResting(std::vector<Order> &out, uint64_t &seq,
                   std::deque<CancelableOrder> &resting, std::mt19937 &rng) {
  if (resting.empty())
    return false;

  std::uniform_int_distribution<size_t> pick(0, resting.size() - 1);
  const size_t idx = pick(rng);
  const CancelableOrder target = resting[idx];
  out.push_back(MakeCancelOrder(seq++, target.client, target.instrument,
                                target.seq, target.side));
  resting[idx] = resting.back();
  resting.pop_back();
  return true;
}

// Mixed limit / cancel / cross flow modeled after bot_fleet steady + spike phases.
void GenerateMarketTraffic(std::vector<Order> &out, uint64_t &seq,
                           std::deque<CancelableOrder> &resting,
                           std::mt19937 &rng, size_t count, bool spike_mode) {
  std::uniform_int_distribution<int> instr_dist(1, kNumInstruments);
  std::uniform_int_distribution<int> client_dist(1, kNumClients);
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<int> action_dist(1, 100);
  std::uniform_int_distribution<int> jitter_dist(-40, 40);
  std::uniform_int_distribution<uint32_t> qty_dist(1, spike_mode ? 8 : 25);

  for (size_t i = 0; i < count; ++i) {
    const uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    const uint32_t client = static_cast<uint32_t>(client_dist(rng));
    const Side side = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
    const uint64_t mid = MidFor(instr);
    const int roll = action_dist(rng);

    const int cancel_cutoff = spike_mode ? 12 : 18;
    const int market_cutoff = spike_mode ? 28 : 22;

    if (roll <= cancel_cutoff) {
      if (!CancelResting(out, seq, resting, rng)) {
        const uint64_t price = static_cast<uint64_t>(
            static_cast<int64_t>(mid) + jitter_dist(rng));
        const uint32_t qty = qty_dist(rng);
        const uint64_t new_seq = seq++;
        out.push_back(MakeOrder(new_seq, client, instr, OrderType::LIMIT, price,
                                qty, side));
        TrackResting(resting, {new_seq, client, instr, side});
      }
      continue;
    }

    if (roll <= market_cutoff) {
      const uint32_t qty = qty_dist(rng);
      const uint64_t edge =
          static_cast<uint64_t>(kSpreadTick * (kBookDepth + (spike_mode ? 4 : 2)));
      const uint64_t price =
          (side == Side::BUY) ? mid + edge : mid - edge;
      out.push_back(
          MakeOrder(seq++, client, instr, OrderType::MARKET, price, qty, side));
      continue;
    }

    const uint64_t price =
        static_cast<uint64_t>(static_cast<int64_t>(mid) + jitter_dist(rng));
    const uint32_t qty = qty_dist(rng);
    const uint64_t new_seq = seq++;
    out.push_back(
        MakeOrder(new_seq, client, instr, OrderType::LIMIT, price, qty, side));
    TrackResting(resting, {new_seq, client, instr, side});
  }
}

bool ValidateCancelEncoding(const std::vector<Order> &orders) {
  std::unordered_set<uint64_t> seen;
  seen.reserve(orders.size());

  for (size_t i = 0; i < orders.size(); ++i) {
    const Order &order = orders[i];
    if (order.type == OrderType::CANCEL) {
      if (order.quantity != 0) {
        std::cerr << "[Generator ERROR] Cancel at index " << i
                  << " has non-zero quantity " << order.quantity << "\n";
        return false;
      }
      if (!seen.count(order.price)) {
        std::cerr << "[Generator ERROR] Cancel at index " << i
                  << " price/target_seq " << order.price
                  << " does not reference a prior order\n";
        return false;
      }
    }
    if (order.type == OrderType::LIMIT || order.type == OrderType::MARKET ||
        order.type == OrderType::CANCEL) {
      seen.insert(order.sequence_id);
    }
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const char *output_path = "benchmark_load.bin";
  if (argc >= 2)
    output_path = argv[1];

  std::vector<Order> orders;
  orders.reserve(kTargetOrders + 256);

  uint64_t seq = 1;
  std::deque<CancelableOrder> resting;
  std::mt19937 rng(0xBEE7CAFE);

  SeedBook(orders, seq, resting);

  // ~70% sustained two-sided flow, ~20% open-market burst, ~10% seed overhead.
  const size_t warmup_orders = kTargetOrders / 10;
  const size_t sustain_orders = (kTargetOrders * 7) / 10;
  const size_t spike_orders =
      kTargetOrders - warmup_orders - sustain_orders - orders.size();

  GenerateMarketTraffic(orders, seq, resting, rng, warmup_orders, false);
  GenerateMarketTraffic(orders, seq, resting, rng, sustain_orders, false);
  GenerateMarketTraffic(orders, seq, resting, rng, spike_orders, true);

  if (!ValidateCancelEncoding(orders)) {
    return 1;
  }

  FILE *file = fopen(output_path, "wb");
  if (!file) {
    std::cerr << "Failed to open " << output_path << " for writing\n";
    return 1;
  }

  const size_t written =
      fwrite(orders.data(), sizeof(Order), orders.size(), file);
  fclose(file);

  if (written != orders.size()) {
    std::cerr << "Short write to " << output_path << "\n";
    return 1;
  }

  std::cout << "[Generator] Wrote " << orders.size() << " orders ("
            << (orders.size() * sizeof(Order)) << " bytes) to " << output_path
            << "\n";
  return 0;
}
