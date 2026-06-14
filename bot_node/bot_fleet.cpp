#include "Order.h"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <cstdint>
#include <vector>

using Clock = std::chrono::steady_clock;

static const char *ENGINE_HOST = "127.0.0.1";
static uint16_t ENGINE_PORT = 8080;

static constexpr int NUM_INSTRUMENTS = 4;
static constexpr int BASE = 10000;
static constexpr int BOOK_DEPTH = 10;
static constexpr int SPREAD_TICK = 2;
static constexpr int LAYER_DEPTH = 35;
static constexpr int PHASE1_DURATION_SEC = 10;

static constexpr int MM_OPS = 4500;
static constexpr int TAKER_OPS = 3500;
static constexpr int CANCEL_OPS = 3500;
static constexpr int WASH_OPS = 2500;
static constexpr int FLOOD_OPS = 5000;
static constexpr int LAYER_OPS = 3500;
static constexpr int HOTQUOTE_OPS = 4500;

static constexpr uint64_t SEQ_MIN = 8000000;
static constexpr uint64_t SEQ_MAX = 9999999;

static std::atomic<bool> SHUTDOWN{false};
static std::atomic<uint64_t> SEQ{1};
static std::atomic<uint64_t> NETWORK_ORDERS_SENT{0};
static std::atomic<uint64_t> GLOBAL_SEED{0};

inline uint64_t BotSeed(uint32_t client_id) {
  return client_id * 6364136223846793005ULL ^
         GLOBAL_SEED.load(std::memory_order_relaxed);
}

void RequestGracefulShutdown(const char *reason) {
  if (SHUTDOWN.exchange(true))
    return;
  std::cout << "[Fleet] " << reason << "\n";
}

inline uint64_t NextSeq() {
  return SEQ.fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t MidFor(uint16_t instr) {
  return static_cast<uint64_t>(BASE) * instr;
}

int ConnectToEngine() {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ENGINE_PORT);
  inet_pton(AF_INET, ENGINE_HOST, &addr.sin_addr);

  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

static Order MakeOrder(uint64_t seq, uint32_t client, uint16_t instr,
                       OrderType type, uint64_t price, uint32_t qty,
                       Side side) {
  Order order{};
  order.sequence_id = seq;
  order.client_id = client;
  order.instrument_id = instr;
  order.type = type;
  order.price = price;
  order.quantity = (type == OrderType::CANCEL) ? 0 : qty;
  order.side = side;
  return order;
}

static bool SendOrderFrame(int sock, const Order &order) {
  if (send(sock, &order, sizeof(order), 0) !=
      static_cast<ssize_t>(sizeof(order))) {
    return false;
  }
  NETWORK_ORDERS_SENT.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool SendOrder(int sock, uint64_t seq, uint32_t client, uint16_t instr,
               uint64_t price, uint32_t qty, Side side) {
  const Order order =
      MakeOrder(seq, client, instr, OrderType::LIMIT, price, qty, side);
  if (!SendOrderFrame(sock, order)) {
    RequestGracefulShutdown("Engine severed connection during send.");
    return false;
  }
  return true;
}

bool SendMarket(int sock, uint64_t seq, uint32_t client, uint16_t instr,
                uint64_t price, uint32_t qty, Side side) {
  const Order order =
      MakeOrder(seq, client, instr, OrderType::MARKET, price, qty, side);
  if (!SendOrderFrame(sock, order)) {
    RequestGracefulShutdown("Engine severed connection during market send.");
    return false;
  }
  return true;
}

bool SendCancel(int sock, uint64_t target_seq, uint32_t client, uint16_t instr,
                Side side) {
  const Order order = MakeOrder(NextSeq(), client, instr, OrderType::CANCEL,
                                target_seq, 0, side);
  if (!SendOrderFrame(sock, order)) {
    RequestGracefulShutdown("Engine severed connection during cancel.");
    return false;
  }
  return true;
}

struct RateLimiter {
  int target_per_sec;
  int ops_this_window{0};
  Clock::time_point window_start{Clock::now()};

  explicit RateLimiter(int ops_per_sec) : target_per_sec(ops_per_sec) {}

  void Throttle() {
    ops_this_window++;
    if (ops_this_window >= target_per_sec) {
      auto elapsed = Clock::now() - window_start;
      auto budget = std::chrono::seconds(1);
      if (elapsed < budget)
        std::this_thread::sleep_for(budget - elapsed);
      window_start = Clock::now();
      ops_this_window = 0;
    }
  }
};

struct RestingOrder {
  uint64_t seq_id;
  Side side;
  uint16_t instrument_id;
};

void RunFixedDurationController(uint64_t duration_ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  RequestGracefulShutdown("Phase 1 correctness load complete.");
}

// --- Bot 1: Market Maker ---------------------------------------------------

void MarketMakerBot(uint32_t client_id, uint16_t instr_id) {
  int sock = ConnectToEngine();
  if (sock < 0) {
    std::cerr << "[MM " << client_id << "] Connection failed\n";
    return;
  }

  static constexpr int REFRESH_MS = 2000;
  uint64_t mid = MidFor(instr_id);
  std::mt19937 rng(BotSeed(client_id));
  std::vector<RestingOrder> resting;
  resting.reserve(BOOK_DEPTH * 2);
  RateLimiter rl(MM_OPS);
  auto last_refresh = Clock::now();

  auto post_book = [&]() {
    for (int i = 1; i <= BOOK_DEPTH && !SHUTDOWN.load(); ++i) {
      uint64_t bid_price = mid - static_cast<uint64_t>(i * SPREAD_TICK);
      uint64_t ask_price = mid + static_cast<uint64_t>(i * SPREAD_TICK);
      uint32_t qty = static_cast<uint32_t>(10 + (BOOK_DEPTH - i) * 5);

      uint64_t bid_seq = NextSeq();
      SendOrder(sock, bid_seq, client_id, instr_id, bid_price, qty, Side::BUY);
      resting.push_back({bid_seq, Side::BUY, instr_id});
      rl.Throttle();

      uint64_t ask_seq = NextSeq();
      SendOrder(sock, ask_seq, client_id, instr_id, ask_price, qty, Side::SELL);
      resting.push_back({ask_seq, Side::SELL, instr_id});
      rl.Throttle();
    }
  };

  auto cancel_all = [&]() {
    for (const auto &o : resting)
      SendCancel(sock, o.seq_id, client_id, o.instrument_id, o.side);
    resting.clear();
  };

  while (!SHUTDOWN.load()) {
    auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refresh)
            .count() > REFRESH_MS) {
      cancel_all();
      last_refresh = now;
    }
    post_book();

    uint64_t drift = (rng() % 3) * SPREAD_TICK;
    if (rng() % 2 == 0)
      mid += drift;
    else
      mid = (mid > drift) ? mid - drift : mid;
    mid = std::max(mid, static_cast<uint64_t>(BASE / 2));
  }

  cancel_all();
  close(sock);
}

// --- Bot 2: Aggressive Taker ------------------------------------------------

void AggressiveTakerBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);
  RateLimiter rl(TAKER_OPS);

  while (!SHUTDOWN.load()) {
    uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    uint64_t mid = MidFor(instr);
    bool do_sweep = (rng() % 4 == 0);

    if (do_sweep) {
      uint64_t edge = static_cast<uint64_t>(SPREAD_TICK * (BOOK_DEPTH + 2));
      SendMarket(sock, NextSeq(), client_id, instr, mid + edge, 500, Side::BUY);
      rl.Throttle();
      SendMarket(sock, NextSeq(), client_id, instr, mid - edge, 500, Side::SELL);
      rl.Throttle();
    } else {
      uint64_t tick = static_cast<uint64_t>(SPREAD_TICK);
      SendOrder(sock, NextSeq(), client_id, instr, mid + tick + 1, 10, Side::BUY);
      rl.Throttle();
      SendOrder(sock, NextSeq(), client_id, instr, mid - tick - 1, 10, Side::SELL);
      rl.Throttle();
    }
  }
  close(sock);
}

// --- Bot 3: Cancel Spammer --------------------------------------------------

void CancelSpammerBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);
  std::uniform_int_distribution<uint64_t> phantom_dist(SEQ_MIN, SEQ_MAX);
  RateLimiter rl(CANCEL_OPS);

  while (!SHUTDOWN.load()) {
    uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    uint64_t mid = MidFor(instr);

    SendCancel(sock, phantom_dist(rng), client_id, instr, Side::BUY);
    rl.Throttle();

    uint64_t real_seq = NextSeq();
    SendOrder(sock, real_seq, client_id, instr, mid - 200, 1, Side::BUY);
    rl.Throttle();
    SendCancel(sock, real_seq, client_id, instr, Side::BUY);
    rl.Throttle();

    if (rng() % 8 == 0) {
      SendCancel(sock, real_seq, client_id, instr, Side::BUY);
      rl.Throttle();
    }
  }
  close(sock);
}

// --- Bot 4: Wash Trader -----------------------------------------------------

void WashTraderBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);
  RateLimiter rl(WASH_OPS);

  while (!SHUTDOWN.load()) {
    uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    uint64_t mid = MidFor(instr);
    uint64_t bid_seq = NextSeq();
    SendOrder(sock, bid_seq, client_id, instr, mid, 10, Side::BUY);
    rl.Throttle();
    SendOrder(sock, NextSeq(), client_id, instr, mid - 1, 10, Side::SELL);
    rl.Throttle();
    SendCancel(sock, bid_seq, client_id, instr, Side::BUY);
    rl.Throttle();
  }
  close(sock);
}

// --- Bot 5: Flood -----------------------------------------------------------

void FloodBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);
  std::uniform_int_distribution<uint64_t> price_dist(1000, 5000);
  RateLimiter rl(FLOOD_OPS);

  while (!SHUTDOWN.load()) {
    uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    Side side = (rng() % 2) ? Side::BUY : Side::SELL;
    SendOrder(sock, NextSeq(), client_id, instr, price_dist(rng), 1, side);
    rl.Throttle();
  }
  close(sock);
}

// --- Bot 6: Layering (deep book stacking) -----------------------------------

void LayeringBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);
  std::vector<RestingOrder> resting;
  resting.reserve(LAYER_DEPTH * 2);
  RateLimiter rl(LAYER_OPS);
  auto last_bulk_cancel = Clock::now();

  while (!SHUTDOWN.load()) {
    uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    uint64_t mid = MidFor(instr);

    for (int i = 1; i <= LAYER_DEPTH && !SHUTDOWN.load(); ++i) {
      uint64_t bid = mid - static_cast<uint64_t>(i * SPREAD_TICK * 2);
      uint64_t ask = mid + static_cast<uint64_t>(i * SPREAD_TICK * 2);
      uint64_t bseq = NextSeq();
      SendOrder(sock, bseq, client_id, instr, bid, 1, Side::BUY);
      resting.push_back({bseq, Side::BUY, instr});
      rl.Throttle();
      uint64_t aseq = NextSeq();
      SendOrder(sock, aseq, client_id, instr, ask, 1, Side::SELL);
      resting.push_back({aseq, Side::SELL, instr});
      rl.Throttle();
    }

    auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_bulk_cancel)
            .count() > 10000 &&
        resting.size() > 40) {
      const size_t drop = resting.size() / 5;
      for (size_t i = 0; i < drop; ++i)
        SendCancel(sock, resting[i].seq_id, client_id, resting[i].instrument_id,
                   resting[i].side);
      resting.erase(resting.begin(), resting.begin() + static_cast<long>(drop));
      last_bulk_cancel = now;
    }
  }

  for (const auto &o : resting)
    SendCancel(sock, o.seq_id, client_id, o.instrument_id, o.side);
  close(sock);
}

// --- Bot 7: Hot Quote (cancel-replace hot path) -----------------------------

void HotQuoteBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);
  RateLimiter rl(HOTQUOTE_OPS);

  while (!SHUTDOWN.load()) {
    uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
    uint64_t mid = MidFor(instr);
    Side side = (rng() % 2) ? Side::BUY : Side::SELL;
    uint64_t price = (side == Side::BUY) ? mid - SPREAD_TICK : mid + SPREAD_TICK;

    for (int i = 0; i < 4 && !SHUTDOWN.load(); ++i) {
      uint64_t seq = NextSeq();
      SendOrder(sock, seq, client_id, instr, price, 5, side);
      rl.Throttle();
      SendCancel(sock, seq, client_id, instr, side);
      rl.Throttle();
      price = (side == Side::BUY)
                  ? (price > SPREAD_TICK ? price - SPREAD_TICK : price)
                  : price + SPREAD_TICK;
    }
  }
  close(sock);
}

// --- Bot 8: Burst Injector --------------------------------------------------

void BurstInjectorBot(uint32_t client_id) {
  int sock = ConnectToEngine();
  if (sock < 0)
    return;

  std::mt19937 rng(BotSeed(client_id));
  std::uniform_int_distribution<int> instr_dist(1, NUM_INSTRUMENTS);

  while (!SHUTDOWN.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (SHUTDOWN.load())
      break;

    const int burst_count = 400 + static_cast<int>(rng() % 400);
    const auto burst_end = Clock::now() + std::chrono::milliseconds(150);
    int sent = 0;

    while (!SHUTDOWN.load() && sent < burst_count &&
           Clock::now() < burst_end) {
      uint16_t instr = static_cast<uint16_t>(instr_dist(rng));
      uint64_t mid = MidFor(instr);
      Side side = (rng() % 2) ? Side::BUY : Side::SELL;
      SendOrder(sock, NextSeq(), client_id, instr,
                mid + static_cast<uint64_t>((rng() % 20) * SPREAD_TICK), 1,
                side);
      ++sent;
    }
  }
  close(sock);
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  if (argc >= 3) {
    ENGINE_HOST = argv[1];
    ENGINE_PORT = static_cast<uint16_t>(std::stoi(argv[2]));
  }

  std::random_device rd;
  const uint64_t load_seed =
      (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
  GLOBAL_SEED.store(load_seed, std::memory_order_relaxed);
  SEQ.store(1);

  std::cout << "\n[Fleet] correctness load → " << ENGINE_HOST << ":"
            << ENGINE_PORT << " (" << PHASE1_DURATION_SEC << "s)\n";

  std::vector<std::thread> threads;
  for (int instr = 1; instr <= NUM_INSTRUMENTS; ++instr) {
    threads.emplace_back(MarketMakerBot, 100 + instr, static_cast<uint16_t>(instr));
    threads.emplace_back(MarketMakerBot, 200 + instr, static_cast<uint16_t>(instr));
  }
  threads.emplace_back(AggressiveTakerBot, 301);
  threads.emplace_back(AggressiveTakerBot, 302);
  threads.emplace_back(AggressiveTakerBot, 303);
  threads.emplace_back(AggressiveTakerBot, 304);
  threads.emplace_back(CancelSpammerBot, 401);
  threads.emplace_back(CancelSpammerBot, 402);
  threads.emplace_back(CancelSpammerBot, 403);
  threads.emplace_back(CancelSpammerBot, 404);
  threads.emplace_back(WashTraderBot, 501);
  threads.emplace_back(WashTraderBot, 502);
  threads.emplace_back(FloodBot, 601);
  threads.emplace_back(FloodBot, 602);
  threads.emplace_back(FloodBot, 603);
  threads.emplace_back(FloodBot, 604);
  threads.emplace_back(LayeringBot, 701);
  threads.emplace_back(LayeringBot, 702);
  threads.emplace_back(LayeringBot, 703);
  threads.emplace_back(LayeringBot, 704);
  threads.emplace_back(HotQuoteBot, 801);
  threads.emplace_back(HotQuoteBot, 802);
  threads.emplace_back(HotQuoteBot, 803);
  threads.emplace_back(HotQuoteBot, 804);
  threads.emplace_back(BurstInjectorBot, 901);
  threads.emplace_back(BurstInjectorBot, 902);
  threads.emplace_back(BurstInjectorBot, 903);
  threads.emplace_back(BurstInjectorBot, 904);

  RunFixedDurationController(static_cast<uint64_t>(PHASE1_DURATION_SEC) * 1000);

  for (auto &t : threads)
    if (t.joinable())
      t.join();

  std::cout << "[Fleet] Orders sent: "
            << NETWORK_ORDERS_SENT.load() << "\n";
  return 0;
}
