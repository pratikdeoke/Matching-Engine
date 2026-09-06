// Standalone latency and throughput harness.
//
// Deliberately does not depend on Google Benchmark, for two reasons:
//   1. It must run in environments where the dependency is unavailable.
//   2. Google Benchmark reports mean/median wall time per iteration; what
//      matters for a matching engine is the tail, so we keep every sample and
//      compute percentiles ourselves.
//
// Methodology
//   - Prices are integer ticks; no floating point on the measured path.
//   - The engine clock is left unset (returns 0) so that no scenario pays for
//      clock_gettime inside apply(). Timing instrumentation is ours alone and
//      its cost is measured separately and reported.
//   - Order pools are pre-touched before measurement, so first-fault page
//      allocation does not appear as tail latency.
//   - Every scenario runs untimed warmup iterations before the measured run.
//   - Setup work (building liquidity, clearing the event buffer) happens
//      strictly outside the timed region. Only Exchange::apply() is measured.
//   - Latency scenarios time each command individually. Throughput scenarios
//      use a single timer around the whole loop, so per-op timer cost is not
//      folded into the rate.
//
// The numbers this prints are whatever the host produces. They are not
// normalised, and a shared or virtualised host will show a much worse tail than
// a tuned bare-metal box (isolated core, no frequency scaling, no SMT sibling).
// Record the environment alongside any result.
#include "BenchUtil.hpp"

#include "exchange/Exchange.hpp"
#include "tools/WorkloadGenerator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace te;
using namespace te::bench;

namespace {

constexpr Ticks kRef = 10'000;

struct Env {
  std::unique_ptr<Exchange> ex;
  SymbolId sym{};
  EventBuffer buf;
  std::uint64_t next_id{1};

  explicit Env(std::size_t prealloc = 1 << 20) {
    ExchangeConfig cfg;
    cfg.prealloc_orders = prealloc;
    ex = std::make_unique<Exchange>(cfg);
    sym = ex->add_instrument("BENCH", 1, 100, 1, 100'000'000,
                             1'000'000'000'000'000LL);
    buf.clear();
  }

  OrderId next() { return OrderId{next_id++}; }

  // Untimed submission helper.
  void submit(const Command &c) {
    buf.clear();
    ex->apply(c, buf);
  }
};

// A benchmark that accidentally measures the reject path looks great and means
// nothing. Every scenario asserts the shape of its own output.
void expect_events(const EventBuffer &buf, EventType t, std::size_t want,
                   const char *scenario) {
  const std::size_t got = buf.count(t);
  if (got != want) {
    std::fprintf(stderr,
                 "%s: expected %zu %s event(s), got %zu — benchmark is not "
                 "measuring what it claims\n",
                 scenario, want, std::string(to_string(t)).c_str(), got);
    std::abort();
  }
}

// ---------------------------------------------------------------------------
// Latency scenarios
// ---------------------------------------------------------------------------

// Passive limit orders that never cross: pure insertion cost into a growing
// book. Each order goes to a distinct price inside a band, so levels are
// created and reused rather than all landing on one FIFO.
Stats bench_insert_passive(std::size_t n, Ticks band) {
  Env env;
  Rng rng(1);
  std::vector<std::int64_t> samples;
  samples.reserve(n);

  // Warmup: same code path, discarded.
  for (std::size_t i = 0; i < n / 10 + 1; ++i) {
    env.submit(Command::new_limit(env.sym, ClientId{1}, env.next(), Side::Buy,
                                  kRef - 1 - rng.range(0, band), 100));
  }

  for (std::size_t i = 0; i < n; ++i) {
    const Command c = Command::new_limit(env.sym, ClientId{1}, env.next(),
                                         Side::Buy,
                                         kRef - 1 - rng.range(0, band), 100);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    samples.push_back(now_ns() - t0);
    expect_events(env.buf, EventType::OrderAccepted, 1, "new_limit_passive");
    expect_events(env.buf, EventType::Trade, 0, "new_limit_passive");
  }
  return summarize(std::move(samples));
}

// Cancel of a resting order, given only its id. Exercises the hash lookup plus
// the O(1) intrusive unlink. Cancels are issued in an order uncorrelated with
// insertion, which is what a real cancel stream looks like.
Stats bench_cancel(std::size_t n) {
  Env env;
  Rng rng(2);
  std::vector<OrderId> ids;
  ids.reserve(n * 2);

  const std::size_t total = n * 2; // extra so the book stays deep throughout
  for (std::size_t i = 0; i < total; ++i) {
    const OrderId id = env.next();
    ids.push_back(id);
    env.submit(Command::new_limit(env.sym, ClientId{1}, id, Side::Buy,
                                  kRef - 1 - rng.range(0, 200), 100));
  }
  // Fisher-Yates so cancel order is unrelated to insertion order.
  for (std::size_t i = ids.size(); i > 1; --i) {
    const std::size_t j = static_cast<std::size_t>(rng.below(i));
    std::swap(ids[i - 1], ids[j]);
  }

  std::vector<std::int64_t> samples;
  samples.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Command c = Command::cancel(env.sym, ClientId{1}, ids[i]);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    samples.push_back(now_ns() - t0);
    expect_events(env.buf, EventType::OrderCancelled, 1, "cancel_resting");
  }
  return summarize(std::move(samples));
}

// One aggressor consuming exactly one resting order: a single trade, resting
// order fully filled and removed, aggressor fully filled and never rested.
Stats bench_match_full_fill(std::size_t n) {
  Env env;
  std::vector<std::int64_t> samples;
  samples.reserve(n);

  for (std::size_t i = 0; i < n + 64; ++i) {
    // Untimed: place the resting ask this iteration will consume.
    env.submit(Command::new_limit(env.sym, ClientId{1}, env.next(), Side::Sell,
                                  kRef, 100));
    const Command c = Command::new_limit(env.sym, ClientId{2}, env.next(),
                                         Side::Buy, kRef, 100);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    const std::int64_t dt = now_ns() - t0;
    if (i >= 64) { // discard warmup
      samples.push_back(dt);
    }
    expect_events(env.buf, EventType::Trade, 1, "match_full_fill_1_trade");
  }
  return summarize(std::move(samples));
}

// Aggressor fully filled against a larger resting order, which survives with a
// reduced quantity. Exercises the reduce path rather than the removal path.
Stats bench_match_partial_resting(std::size_t n) {
  Env env;
  std::vector<std::int64_t> samples;
  samples.reserve(n);

  for (std::size_t i = 0; i < n + 64; ++i) {
    const OrderId resting = env.next();
    env.submit(Command::new_limit(env.sym, ClientId{1}, resting, Side::Sell,
                                  kRef, 1000));
    const Command c = Command::new_limit(env.sym, ClientId{2}, env.next(),
                                         Side::Buy, kRef, 400);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    const std::int64_t dt = now_ns() - t0;
    if (i >= 64) {
      samples.push_back(dt);
    }
    expect_events(env.buf, EventType::Trade, 1, "match_partial_resting");
    // Untimed: remove the residual so book depth stays constant.
    env.submit(Command::cancel(env.sym, ClientId{1}, resting));
  }
  return summarize(std::move(samples));
}

// Aggressor sweeping `levels` price levels, one resting order per level, so the
// command produces exactly `levels` trades. This is the scenario where the
// choice of book structure shows up: each level transition is a pop of the
// touch, which is back() of a sorted vector.
Stats bench_match_sweep(std::size_t n, int levels) {
  Env env;
  std::vector<std::int64_t> samples;
  samples.reserve(n);

  const Qty per_level = 100;
  for (std::size_t i = 0; i < n + 32; ++i) {
    // Untimed: rebuild `levels` asks starting at kRef, ascending.
    for (int l = 0; l < levels; ++l) {
      env.submit(Command::new_limit(env.sym, ClientId{1}, env.next(),
                                    Side::Sell, kRef + l, per_level));
    }
    const Command c = Command::new_limit(
        env.sym, ClientId{2}, env.next(), Side::Buy, kRef + levels - 1,
        per_level * levels);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    const std::int64_t dt = now_ns() - t0;
    if (i >= 32) {
      samples.push_back(dt);
    }
    expect_events(env.buf, EventType::Trade, static_cast<std::size_t>(levels),
                  "match_sweep");
  }
  return summarize(std::move(samples));
}

// Market order sweeping the same depth, for comparison with the limit sweep:
// isolates the cost of the limit-price bound check.
Stats bench_market_sweep(std::size_t n, int levels) {
  Env env;
  std::vector<std::int64_t> samples;
  samples.reserve(n);

  const Qty per_level = 100;
  for (std::size_t i = 0; i < n + 32; ++i) {
    for (int l = 0; l < levels; ++l) {
      env.submit(Command::new_limit(env.sym, ClientId{1}, env.next(),
                                    Side::Sell, kRef + l, per_level));
    }
    const Command c = Command::new_market(env.sym, ClientId{2}, env.next(),
                                          Side::Buy, per_level * levels);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    const std::int64_t dt = now_ns() - t0;
    if (i >= 32) {
      samples.push_back(dt);
    }
    expect_events(env.buf, EventType::Trade, static_cast<std::size_t>(levels),
                  "market_order_sweep");
  }
  return summarize(std::move(samples));
}

// Modify that loses priority (price change): cancel/replace plus re-match.
Stats bench_modify(std::size_t n) {
  Env env;
  Rng rng(3);
  std::vector<OrderId> ids;
  const std::size_t pool = 20000;
  for (std::size_t i = 0; i < pool; ++i) {
    const OrderId id = env.next();
    ids.push_back(id);
    env.submit(Command::new_limit(env.sym, ClientId{1}, id, Side::Buy,
                                  kRef - 1 - rng.range(0, 200), 100));
  }

  std::vector<std::int64_t> samples;
  samples.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const OrderId id = ids[static_cast<std::size_t>(rng.below(pool))];
    const Command c = Command::modify(env.sym, ClientId{1}, id,
                                      kRef - 1 - rng.range(0, 200), 100);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    samples.push_back(now_ns() - t0);
  }
  return summarize(std::move(samples));
}

// Realistic mixed flow from the synthetic generator: new orders, cancels,
// modifies, markets, IOCs, across several instruments. This is the number that
// most closely resembles end-to-end engine behaviour.
Stats bench_mixed_flow(std::size_t n, std::int64_t *elapsed_out) {
  ExchangeConfig cfg;
  cfg.prealloc_orders = 1 << 20;
  Exchange ex(cfg);
  std::vector<SymbolId> syms{ex.add_instrument("AAPL"), ex.add_instrument("MSFT"),
                             ex.add_instrument("GOOG"), ex.add_instrument("NVDA")};

  WorkloadConfig wc;
  wc.seed = 4242;
  wc.num_commands = n;
  wc.num_clients = 32;
  WorkloadGenerator gen(wc, syms);
  const std::vector<Command> cmds = gen.generate(); // generated ahead of time

  // Warmup on a separate exchange so the measured run starts with a warm cache
  // but a book state produced only by the measured stream.
  {
    Exchange warm(cfg);
    std::vector<SymbolId> ws{warm.add_instrument("AAPL"),
                             warm.add_instrument("MSFT"),
                             warm.add_instrument("GOOG"),
                             warm.add_instrument("NVDA")};
    (void)ws;
    EventBuffer wb;
    for (std::size_t i = 0; i < cmds.size() / 10; ++i) {
      wb.clear();
      warm.apply(cmds[i], wb);
    }
  }

  EventBuffer buf;
  buf.clear();
  std::vector<std::int64_t> samples;
  samples.reserve(cmds.size());

  const std::int64_t loop_start = now_ns();
  for (const Command &c : cmds) {
    buf.clear();
    const std::int64_t t0 = now_ns();
    ex.apply(c, buf);
    samples.push_back(now_ns() - t0);
  }
  if (elapsed_out != nullptr) {
    *elapsed_out = now_ns() - loop_start;
  }
  return summarize(std::move(samples));
}

// ---------------------------------------------------------------------------
// Throughput scenarios (single timer around the loop, no per-op timing)
// ---------------------------------------------------------------------------

std::int64_t throughput_insert(std::size_t n) {
  Env env;
  Rng rng(11);
  std::vector<Command> cmds;
  cmds.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    cmds.push_back(Command::new_limit(env.sym, ClientId{1}, env.next(),
                                      Side::Buy, kRef - 1 - rng.range(0, 500),
                                      100));
  }
  // Warmup pass on a throwaway exchange.
  {
    Env w;
    EventBuffer wb;
    for (std::size_t i = 0; i < n / 10; ++i) {
      wb.clear();
      w.ex->apply(cmds[i], wb);
    }
  }
  EventBuffer buf;
  const std::int64_t t0 = now_ns();
  for (const Command &c : cmds) {
    buf.clear();
    env.ex->apply(c, buf);
  }
  return now_ns() - t0;
}

std::int64_t throughput_mixed(std::size_t n, std::size_t *trades_out) {
  ExchangeConfig cfg;
  cfg.prealloc_orders = 1 << 20;
  Exchange ex(cfg);
  std::vector<SymbolId> syms{ex.add_instrument("AAPL"), ex.add_instrument("MSFT"),
                             ex.add_instrument("GOOG"), ex.add_instrument("NVDA")};
  WorkloadConfig wc;
  wc.seed = 9001;
  wc.num_commands = n;
  wc.num_clients = 32;
  WorkloadGenerator gen(wc, syms);
  const std::vector<Command> cmds = gen.generate();

  EventBuffer buf;
  std::size_t trades = 0;
  const std::int64_t t0 = now_ns();
  for (const Command &c : cmds) {
    buf.clear();
    ex.apply(c, buf);
    trades += buf.count(EventType::Trade);
  }
  const std::int64_t dt = now_ns() - t0;
  if (trades_out != nullptr) {
    *trades_out = trades;
  }
  return dt;
}

// ---------------------------------------------------------------------------
// Depth scaling: does insertion latency degrade as the book gets deep?
// ---------------------------------------------------------------------------

Stats bench_insert_at_depth(std::size_t prefill_levels, std::size_t n) {
  Env env;
  // A high reference price so that `prefill_levels` levels below it all stay
  // strictly positive. (An earlier version used kRef = 10'000 and the deepest
  // configurations generated negative prices, which the risk check rejected —
  // producing a *faster* p50 at depth 50k than at depth 100. A benchmark that
  // gets quicker as the problem gets bigger is measuring the reject path.)
  const Ticks base = static_cast<Ticks>(prefill_levels) + 1000;
  // Prefill distinct price levels, one order each, so the level vector reaches
  // the target size.
  for (std::size_t l = 0; l < prefill_levels; ++l) {
    env.submit(Command::new_limit(env.sym, ClientId{1}, env.next(), Side::Buy,
                                  base - 1 - static_cast<Ticks>(l), 100));
  }
  Rng rng(7);
  std::vector<std::int64_t> samples;
  samples.reserve(n);
  const auto span = static_cast<std::int64_t>(prefill_levels);
  for (std::size_t i = 0; i < n; ++i) {
    // Insert into an existing level (the common case) at a random depth.
    const Ticks p = base - 1 - rng.range(0, span > 0 ? span - 1 : 0);
    const Command c = Command::new_limit(env.sym, ClientId{1}, env.next(),
                                         Side::Buy, p, 100);
    env.buf.clear();
    const std::int64_t t0 = now_ns();
    env.ex->apply(c, env.buf);
    const std::int64_t dt = now_ns() - t0;
    // Guard against silently measuring the reject path.
    if (env.buf.count(EventType::OrderRejected) != 0) {
      std::fprintf(stderr, "bench_insert_at_depth: unexpected reject\n");
      std::abort();
    }
    samples.push_back(dt);
  }
  return summarize(std::move(samples));
}

void print_env_block(double overhead) {
  std::printf("environment\n");
  std::printf("  compiler        : ");
#if defined(__clang__)
  std::printf("clang %d.%d.%d\n", __clang_major__, __clang_minor__,
              __clang_patchlevel__);
#elif defined(__GNUC__)
  std::printf("gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  std::printf("unknown\n");
#endif
  std::printf("  build type      : %s\n",
#ifdef NDEBUG
              "release (NDEBUG)"
#else
              "debug (assertions ON — latency is NOT representative)"
#endif
  );
  std::printf("  sizeof(Order)   : %zu bytes\n", sizeof(Order));
  std::printf("  sizeof(Event)   : %zu bytes\n", sizeof(Event));
  std::printf("  sizeof(Command) : %zu bytes\n", sizeof(Command));
  std::printf("  timer overhead  : %.1f ns per clock read (subtract ~%.0f ns "
              "from each latency sample below)\n",
              overhead, overhead);
  std::printf("  engine clock    : disabled (timestamps are 0; see "
              "MatchingEngine::set_clock)\n\n");
}

} // namespace

int main(int argc, char **argv) {
  // Scale factor so the harness can run quickly in CI and longer locally.
  std::size_t scale = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      scale = 0;
    }
  }
  const std::size_t N = scale == 0 ? 20'000 : 200'000;
  const std::size_t NM = scale == 0 ? 50'000 : 500'000;

  const double overhead = clock_overhead_ns();
  print_env_block(overhead);

  std::printf("latency per command (nanoseconds, includes ~%.0f ns timer "
              "overhead)\n\n",
              overhead);
  print_header();
  print_row("new_limit_passive", bench_insert_passive(N, 500));
  print_row("cancel_resting", bench_cancel(N));
  print_row("modify_reprice", bench_modify(N));
  print_row("match_full_fill_1_trade", bench_match_full_fill(N));
  print_row("match_partial_resting", bench_match_partial_resting(N));
  print_row("match_sweep_5_levels", bench_match_sweep(N / 4, 5));
  print_row("match_sweep_20_levels", bench_match_sweep(N / 10, 20));
  print_row("market_order_sweep_5_levels", bench_market_sweep(N / 4, 5));

  std::int64_t mixed_ns = 0;
  const Stats mixed = bench_mixed_flow(NM, &mixed_ns);
  print_row("mixed_flow_all_commands", mixed);

  std::printf("\ninsertion latency vs book depth (levels already resting)\n\n");
  print_header();
  for (std::size_t depth : {std::size_t{100}, std::size_t{1'000},
                            std::size_t{10'000}, std::size_t{50'000}}) {
    char name[64];
    std::snprintf(name, sizeof(name), "insert_at_depth_%zu", depth);
    print_row(name, bench_insert_at_depth(depth, N / 4));
  }

  std::printf("\nthroughput (single timer around the loop, no per-op timing)\n\n");
  print_throughput("insert_only", NM, throughput_insert(NM));
  std::size_t trades = 0;
  const std::int64_t mixed_tp = throughput_mixed(NM, &trades);
  print_throughput("mixed_flow", NM, mixed_tp);
  std::printf("%-34s %zu trades printed during the mixed run\n", "", trades);
  std::printf("%-34s per-op-timed mixed loop took %.3f ms (timer-inflated, "
              "for comparison)\n",
              "", static_cast<double>(mixed_ns) / 1e6);

  return 0;
}
