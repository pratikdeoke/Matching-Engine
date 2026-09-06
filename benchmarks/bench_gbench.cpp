// Google Benchmark targets.
//
// Built only when Google Benchmark is available (see CMakeLists.txt). The
// standalone harness in bench_latency.cpp is the primary tool because it
// reports percentiles; this file exists so the project plugs into the standard
// tooling — Google Benchmark's statistical repetition handling, its JSON
// output for tracking regressions across commits, and its comparison scripts.
//
// Note on what is measured: every fixture builds its state in the untimed
// portion of the loop using PauseTiming/ResumeTiming, so the reported time is
// the engine command only. PauseTiming itself has overhead, which is why the
// sweep fixtures amortise setup across a batch rather than pausing per
// iteration where possible.
#include <benchmark/benchmark.h>

#include "exchange/Exchange.hpp"
#include "tools/WorkloadGenerator.hpp"

using namespace te;

namespace {

constexpr Ticks kRef = 10'000;

struct Fixture {
  std::unique_ptr<Exchange> ex;
  SymbolId sym{};
  EventBuffer buf;
  std::uint64_t next_id{1};

  Fixture() {
    ExchangeConfig cfg;
    cfg.prealloc_orders = 1 << 20;
    ex = std::make_unique<Exchange>(cfg);
    sym = ex->add_instrument("BENCH", 1, 100, 1, 100'000'000,
                             1'000'000'000'000'000LL);
  }
  OrderId next() { return OrderId{next_id++}; }
  void submit(const Command &c) {
    buf.clear();
    ex->apply(c, buf);
  }
};

// Passive insertion into a widening book.
void BM_NewLimitPassive(benchmark::State &state) {
  Fixture f;
  Rng rng(1);
  for (auto _ : state) {
    const Command c = Command::new_limit(f.sym, ClientId{1}, f.next(),
                                         Side::Buy,
                                         kRef - 1 - rng.range(0, 500), 100);
    f.buf.clear();
    f.ex->apply(c, f.buf);
    benchmark::DoNotOptimize(f.buf.size());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NewLimitPassive);

// Cancel by id. Liquidity is rebuilt in batches so the timed region contains
// only cancels.
void BM_CancelResting(benchmark::State &state) {
  Fixture f;
  Rng rng(2);
  std::vector<OrderId> ids;
  constexpr std::size_t kBatch = 4096;

  auto refill = [&] {
    ids.clear();
    for (std::size_t i = 0; i < kBatch; ++i) {
      const OrderId id = f.next();
      ids.push_back(id);
      f.submit(Command::new_limit(f.sym, ClientId{1}, id, Side::Buy,
                                  kRef - 1 - rng.range(0, 200), 100));
    }
  };

  state.PauseTiming();
  refill();
  std::size_t i = 0;
  state.ResumeTiming();

  for (auto _ : state) {
    if (i == ids.size()) {
      state.PauseTiming();
      refill();
      i = 0;
      state.ResumeTiming();
    }
    const Command c = Command::cancel(f.sym, ClientId{1}, ids[i++]);
    f.buf.clear();
    f.ex->apply(c, f.buf);
    benchmark::DoNotOptimize(f.buf.size());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CancelResting);

// One aggressor fully consuming one resting order: a single trade.
void BM_MatchFullFill(benchmark::State &state) {
  Fixture f;
  for (auto _ : state) {
    state.PauseTiming();
    f.submit(Command::new_limit(f.sym, ClientId{1}, f.next(), Side::Sell, kRef,
                                100));
    const Command c = Command::new_limit(f.sym, ClientId{2}, f.next(),
                                         Side::Buy, kRef, 100);
    state.ResumeTiming();

    f.buf.clear();
    f.ex->apply(c, f.buf);
    benchmark::DoNotOptimize(f.buf.size());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MatchFullFill);

// Aggressor sweeping N price levels; N is the benchmark argument, so the output
// shows how cost scales with sweep depth.
void BM_MatchSweep(benchmark::State &state) {
  const auto levels = static_cast<int>(state.range(0));
  Fixture f;
  constexpr Qty kPerLevel = 100;
  for (auto _ : state) {
    state.PauseTiming();
    for (int l = 0; l < levels; ++l) {
      f.submit(Command::new_limit(f.sym, ClientId{1}, f.next(), Side::Sell,
                                  kRef + l, kPerLevel));
    }
    const Command c =
        Command::new_limit(f.sym, ClientId{2}, f.next(), Side::Buy,
                           kRef + levels - 1, kPerLevel * levels);
    state.ResumeTiming();

    f.buf.clear();
    f.ex->apply(c, f.buf);
    benchmark::DoNotOptimize(f.buf.size());
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["trades_per_op"] = levels;
}
BENCHMARK(BM_MatchSweep)->Arg(1)->Arg(5)->Arg(20)->Arg(50);

// Realistic mixed flow across four instruments. The command stream is generated
// once, outside the timed region.
void BM_MixedFlow(benchmark::State &state) {
  ExchangeConfig cfg;
  cfg.prealloc_orders = 1 << 20;
  Exchange ex(cfg);
  std::vector<SymbolId> syms{ex.add_instrument("AAPL"), ex.add_instrument("MSFT"),
                             ex.add_instrument("GOOG"),
                             ex.add_instrument("NVDA")};
  WorkloadConfig wc;
  wc.seed = 4242;
  wc.num_commands = 1'000'000;
  wc.num_clients = 32;
  WorkloadGenerator gen(wc, syms);
  const std::vector<Command> cmds = gen.generate();

  EventBuffer buf;
  std::size_t i = 0;
  for (auto _ : state) {
    buf.clear();
    ex.apply(cmds[i], buf);
    benchmark::DoNotOptimize(buf.size());
    if (++i == cmds.size()) {
      i = 0; // wrap; the book is warm and in steady state by now
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MixedFlow);

// Insertion cost as a function of how many price levels already rest. This is
// the scaling behaviour of the sorted level vector.
void BM_InsertAtDepth(benchmark::State &state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  Fixture f;
  const Ticks base = static_cast<Ticks>(depth) + 1000;
  for (std::size_t l = 0; l < depth; ++l) {
    f.submit(Command::new_limit(f.sym, ClientId{1}, f.next(), Side::Buy,
                                base - 1 - static_cast<Ticks>(l), 100));
  }
  Rng rng(7);
  const auto span = static_cast<std::int64_t>(depth);
  for (auto _ : state) {
    const Command c = Command::new_limit(
        f.sym, ClientId{1}, f.next(), Side::Buy,
        base - 1 - rng.range(0, span > 0 ? span - 1 : 0), 100);
    f.buf.clear();
    f.ex->apply(c, f.buf);
    benchmark::DoNotOptimize(f.buf.size());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InsertAtDepth)->Arg(100)->Arg(1000)->Arg(10000)->Arg(50000);

} // namespace

BENCHMARK_MAIN();
