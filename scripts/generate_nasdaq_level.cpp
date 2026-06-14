// Generates a deterministic NASDAQ-style mixtape for one benchmark level.
// Usage: generate_nasdaq_level <out.bin> <multiplier> <baseline_ops>
//        <duration_sec> <start_seq>

#include "Order.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kNumInstruments = 4;
constexpr int kBasePrice = 10000;
constexpr int kBookDepth = 10;
constexpr int kSpreadTick = 2;
constexpr size_t kMaxResting = 150'000;

enum class Session : uint8_t { MM = 0, Taker = 1, Cancel = 2, Burst = 3 };

struct SegmentProfile {
  size_t order_count;
  double cancel_ratio;
  double market_ratio;
  double spread_mult;
  int qty_mean;
  int qty_spread;
};

struct CancelableOrder {
  uint64_t seq;
  uint32_t client;
  uint16_t instrument;
  Side side;
};

uint32_t MakeClientId(Session session, int local) {
  return (static_cast<uint32_t>(session) << 10) |
         static_cast<uint32_t>(local & 0x3FF);
}

Order MakeOrder(uint64_t seq, uint32_t client, uint16_t instr, OrderType type,
                uint64_t price, uint32_t qty, Side side) {
  Order order{};
  order.sequence_id = seq;
  order.client_id = client;
  order.instrument_id = instr;
  order.type = type;
  order.price = price;
  order.quantity = qty;
  order.side = side;
  return order;
}

Order MakeCancelOrder(uint64_t seq, uint32_t client, uint16_t instr,
                      uint64_t target_seq, Side side) {
  Order order{};
  order.sequence_id = seq;
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

bool CancelResting(std::vector<Order> &out, uint64_t &seq,
                   std::deque<CancelableOrder> &resting, std::mt19937 &rng) {
  if (resting.empty())
    return false;
  std::uniform_int_distribution<size_t> pick(0, resting.size() - 1);
  const size_t idx = pick(rng);
  const CancelableOrder target = resting[idx];
  out.push_back(MakeCancelOrder(
      seq++, MakeClientId(Session::Cancel, static_cast<int>(target.client & 0x3FF)),
      target.instrument, target.seq, target.side));
  resting[idx] = resting.back();
  resting.pop_back();
  return true;
}

uint32_t LogNormalQty(std::mt19937 &rng, int mean, int spread) {
  std::lognormal_distribution<double> dist(std::log(static_cast<double>(mean)), 0.6);
  const int qty = static_cast<int>(dist(rng));
  return static_cast<uint32_t>(std::clamp(qty, 1, mean + spread));
}

uint64_t ClusterPrice(std::mt19937 &rng, uint64_t mid, double spread_mult) {
  std::normal_distribution<double> offset(0.0, 8.0 * spread_mult);
  const int64_t raw = static_cast<int64_t>(mid) +
                      static_cast<int64_t>(offset(rng)) * kSpreadTick;
  if (rng() % 100 < 40) {
    const int64_t tick = raw / kSpreadTick;
    return static_cast<uint64_t>(tick * kSpreadTick);
  }
  return static_cast<uint64_t>(std::max<int64_t>(1, raw));
}

Session PickSession(std::mt19937 &rng, const SegmentProfile &seg, int roll) {
  const int cancel_pct = static_cast<int>(seg.cancel_ratio * 100);
  const int market_pct = static_cast<int>(seg.market_ratio * 100);
  if (roll <= cancel_pct)
    return Session::Cancel;
  if (roll <= cancel_pct + market_pct)
    return Session::Taker;
  if (roll <= cancel_pct + market_pct + 8)
    return Session::Burst;
  (void)rng;
  return Session::MM;
}

void EmitBurstCluster(std::vector<Order> &out, uint64_t &seq,
                      std::deque<CancelableOrder> &resting, std::mt19937 &rng,
                      const SegmentProfile &seg) {
  std::uniform_int_distribution<int> instr_dist(1, kNumInstruments);
  const uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
  const uint64_t mid = MidFor(instr);
  const int cluster_size = 80 + static_cast<int>(rng() % 120);

  for (int i = 0; i < cluster_size; ++i) {
    const Side side = (rng() % 2) ? Side::BUY : Side::SELL;
    const uint32_t client = MakeClientId(Session::Burst, static_cast<int>(rng() % 512));
    const uint32_t qty = LogNormalQty(rng, seg.qty_mean, seg.qty_spread);
    const int roll = static_cast<int>(rng() % 100);

    if (roll < static_cast<int>(seg.cancel_ratio * 100)) {
      CancelResting(out, seq, resting, rng);
      continue;
    }

    if (roll < static_cast<int>((seg.cancel_ratio + seg.market_ratio) * 100)) {
      const uint64_t edge =
          static_cast<uint64_t>(kSpreadTick * (kBookDepth + 3) * seg.spread_mult);
      const uint64_t price = (side == Side::BUY) ? mid + edge : mid - edge;
      out.push_back(MakeOrder(seq++, client, instr, OrderType::MARKET, price, qty, side));
      continue;
    }

    const uint64_t price = ClusterPrice(rng, mid, seg.spread_mult);
    const uint64_t new_seq = seq++;
    out.push_back(MakeOrder(new_seq, client, instr, OrderType::LIMIT, price, qty, side));
    TrackResting(resting, {new_seq, client, instr, side});
  }
}

void GenerateSegment(std::vector<Order> &out, uint64_t &seq,
                     std::deque<CancelableOrder> &resting, std::mt19937 &rng,
                     const SegmentProfile &seg) {
  std::uniform_int_distribution<int> instr_dist(1, kNumInstruments);
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<int> action_dist(1, 100);

  const size_t burst_stride = std::max<size_t>(1, seg.order_count / 40);

  for (size_t i = 0; i < seg.order_count; ++i) {
    if (i > 0 && i % burst_stride == 0)
      EmitBurstCluster(out, seq, resting, rng, seg);

    const int roll = action_dist(rng);
    const Session session = PickSession(rng, seg, roll);
    const uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    const Side side = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
    const uint64_t mid = MidFor(instr);
    const uint32_t client = MakeClientId(session, static_cast<int>(rng() % 512));
    const uint32_t qty = LogNormalQty(rng, seg.qty_mean, seg.qty_spread);

    if (session == Session::Cancel) {
      if (!CancelResting(out, seq, resting, rng)) {
        const uint64_t price = ClusterPrice(rng, mid, seg.spread_mult);
        const uint64_t new_seq = seq++;
        out.push_back(MakeOrder(new_seq, client, instr, OrderType::LIMIT, price, 1, side));
        TrackResting(resting, {new_seq, client, instr, side});
      }
      continue;
    }

    if (session == Session::Taker) {
      const uint64_t edge =
          static_cast<uint64_t>(kSpreadTick * (kBookDepth + 2) * seg.spread_mult);
      const uint64_t price = (side == Side::BUY) ? mid + edge : mid - edge;
      out.push_back(MakeOrder(seq++, client, instr, OrderType::MARKET, price, qty, side));
      continue;
    }

    const uint64_t price = ClusterPrice(rng, mid, seg.spread_mult);
    const uint64_t new_seq = seq++;
    out.push_back(MakeOrder(new_seq, client, instr, OrderType::LIMIT, price, qty, side));
    TrackResting(resting, {new_seq, client, instr, side});
  }
}

void SeedBook(std::vector<Order> &out, uint64_t &seq,
              std::deque<CancelableOrder> &resting) {
  for (uint16_t instr = 1; instr <= kNumInstruments; ++instr) {
    const uint64_t mid = MidFor(instr);
    for (int level = 1; level <= kBookDepth; ++level) {
      const uint32_t client = MakeClientId(Session::MM, level + instr * 10);
      const uint32_t qty = static_cast<uint32_t>(10 + level * 3);
      const uint64_t bid_seq = seq++;
      out.push_back(MakeOrder(bid_seq, client, instr, OrderType::LIMIT,
                              mid - static_cast<uint64_t>(level * kSpreadTick), qty,
                              Side::BUY));
      TrackResting(resting, {bid_seq, client, instr, Side::BUY});
      const uint64_t ask_seq = seq++;
      const uint32_t ask_client = MakeClientId(Session::MM, 100 + level + instr * 10);
      out.push_back(MakeOrder(ask_seq, ask_client, instr, OrderType::LIMIT,
                              mid + static_cast<uint64_t>(level * kSpreadTick), qty,
                              Side::SELL));
      TrackResting(resting, {ask_seq, ask_client, instr, Side::SELL});
    }
  }
}

bool ValidateCancelEncoding(const std::vector<Order> &orders) {
  std::unordered_set<uint64_t> seen;
  seen.reserve(orders.size());
  for (const Order &order : orders) {
    if (order.type == OrderType::CANCEL) {
      if (order.quantity != 0)
        return false;
      if (!seen.count(order.price))
        return false;
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
  if (argc < 6) {
    std::cerr << "Usage: generate_nasdaq_level <out.bin> <multiplier> "
                 "<baseline_ops> <duration_sec> <start_seq>\n";
    return 1;
  }

  const char *output_path = argv[1];
  const double multiplier = std::max(0.25, std::stod(argv[2]));
  const int baseline_ops = std::max(1, std::atoi(argv[3]));
  const int duration_sec = std::max(1, std::atoi(argv[4]));
  const uint64_t start_seq = std::strtoull(argv[5], nullptr, 10);

  const size_t budget = static_cast<size_t>(
      static_cast<double>(baseline_ops) * multiplier *
      static_cast<double>(duration_sec) + 0.5);

  std::vector<Order> orders;
  orders.reserve(budget + 256);
  uint64_t seq = start_seq;
  std::deque<CancelableOrder> resting;
  std::mt19937 rng(0x17CABEEFu +
                   static_cast<unsigned>(multiplier * 9973.0));

  SeedBook(orders, seq, resting);

  const size_t flow_budget = budget > orders.size() ? budget - orders.size() : 0;
  const size_t opening = flow_budget / 10;
  const size_t morning = flow_budget * 25 / 100;
  const size_t midday = flow_budget * 15 / 100;
  const size_t afternoon = flow_budget * 25 / 100;
  const size_t closing = flow_budget - opening - morning - midday - afternoon;

  const SegmentProfile segments[] = {
      {opening, 0.85, 0.08, 2.0, 5, 20},
      {morning, 0.65, 0.05, 1.2, 10, 30},
      {midday, 0.55, 0.01, 1.5, 3, 8},
      {afternoon, 0.70, 0.06, 1.1, 12, 35},
      {closing, 0.80, 0.15, 1.3, 15, 50},
  };

  for (const auto &seg : segments) {
    if (seg.order_count == 0)
      continue;
    GenerateSegment(orders, seq, resting, rng, seg);
  }

  if (!ValidateCancelEncoding(orders)) {
    std::cerr << "[GenerateLevel] Cancel validation failed\n";
    return 1;
  }

  FILE *file = std::fopen(output_path, "wb");
  if (!file) {
    std::perror("fopen");
    return 1;
  }

  const size_t written = std::fwrite(orders.data(), sizeof(Order), orders.size(), file);
  std::fclose(file);

  if (written != orders.size()) {
    std::cerr << "[GenerateLevel] Short write\n";
    return 1;
  }

  std::cout << "[GenerateLevel] " << output_path << " | multiplier=" << multiplier
            << "x | orders=" << orders.size()
            << " | target_ops="
            << static_cast<int>(baseline_ops * multiplier + 0.5)
            << " | start_seq=" << start_seq << "\n";
  return 0;
}
