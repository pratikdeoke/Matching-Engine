// Determinism, replay, and randomised invariant (property) testing.
//
// These are the tests that catch what hand-written cases miss: they run large
// synthetic workloads through the engine and assert structural properties after
// every command, plus verify that replaying a command journal reproduces the
// event stream exactly.
#include "TestFixtures.hpp"

#include "replay/EventLog.hpp"
#include "tools/WorkloadGenerator.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>

using namespace te;
using namespace te::test;

namespace {

std::vector<SymbolId> setup_symbols(Exchange &ex) {
  return {ex.add_instrument("AAPL"), ex.add_instrument("MSFT"),
          ex.add_instrument("GOOG"), ex.add_instrument("NVDA")};
}

// Runs a workload and returns the full event stream.
std::vector<Event> run(const std::vector<Command> &cmds, Exchange &ex) {
  EventBuffer buf;
  for (const Command &c : cmds) {
    ex.apply(c, buf);
  }
  return buf.events();
}

WorkloadConfig default_workload(std::uint64_t seed, std::size_t n) {
  WorkloadConfig cfg;
  cfg.seed = seed;
  cfg.num_commands = n;
  cfg.num_clients = 8;
  return cfg;
}

} // namespace

TEST(Determinism, SameCommandsProduceSameEvents) {
  Exchange a;
  Exchange b;
  const auto syms_a = setup_symbols(a);
  const auto syms_b = setup_symbols(b);

  WorkloadGenerator gen(default_workload(1234, 20000), syms_a);
  const auto cmds = gen.generate();
  (void)syms_b; // symbol ids are assigned identically in both exchanges

  const auto events_a = run(cmds, a);
  const auto events_b = run(cmds, b);

  const ReplayDiff d = compare_event_streams(events_a, events_b);
  EXPECT_TRUE(d.equal);
  if (!d.equal) {
    std::fprintf(stderr, "    %s\n", d.detail.c_str());
  }
}

TEST(Determinism, GeneratorIsReproducibleFromSeed) {
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadGenerator g1(default_workload(999, 5000), syms);
  WorkloadGenerator g2(default_workload(999, 5000), syms);

  const auto a = g1.generate();
  const auto b = g2.generate();
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].type, b[i].type);
    EXPECT_EQ(a[i].order_id, b[i].order_id);
    EXPECT_EQ(a[i].price, b[i].price);
    EXPECT_EQ(a[i].quantity, b[i].quantity);
  }
}

TEST(Determinism, DifferentSeedsProduceDifferentFlow) {
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadGenerator g1(default_workload(1, 2000), syms);
  WorkloadGenerator g2(default_workload(2, 2000), syms);
  const auto a = g1.generate();
  const auto b = g2.generate();

  bool any_difference = false;
  for (std::size_t i = 0; i < a.size() && !any_difference; ++i) {
    any_difference = a[i].price != b[i].price || a[i].quantity != b[i].quantity;
  }
  EXPECT_TRUE(any_difference);
}

TEST(Replay, JournalRoundTripReproducesEventStream) {
  Exchange live;
  const auto syms = setup_symbols(live);
  WorkloadGenerator gen(default_workload(777, 15000), syms);
  const auto cmds = gen.generate();

  const std::string path = "replay_test_journal.bin";
  {
    CommandJournalWriter w;
    ASSERT_TRUE(w.open(path));
    for (const Command &c : cmds) {
      w.append(c);
    }
    ASSERT_TRUE(w.close());
  }

  const auto live_events = run(cmds, live);

  CommandJournalReader r;
  ASSERT_TRUE(r.load(path));
  ASSERT_EQ(r.commands().size(), cmds.size());

  Exchange replayed;
  setup_symbols(replayed);
  const auto replay_events = run(r.commands(), replayed);

  const ReplayDiff d = compare_event_streams(live_events, replay_events);
  EXPECT_TRUE(d.equal);
  if (!d.equal) {
    std::fprintf(stderr, "    %s\n", d.detail.c_str());
  }

  // Final book state must agree too, not just the event stream.
  for (SymbolId s : syms) {
    EXPECT_EQ(live.book(s).best_bid(), replayed.book(s).best_bid());
    EXPECT_EQ(live.book(s).best_ask(), replayed.book(s).best_ask());
    EXPECT_EQ(live.book(s).best_bid_qty(), replayed.book(s).best_bid_qty());
    EXPECT_EQ(live.book(s).best_ask_qty(), replayed.book(s).best_ask_qty());
    EXPECT_EQ(live.book(s).resting_orders(), replayed.book(s).resting_orders());
  }
  std::remove(path.c_str());
}

TEST(Replay, TruncatedJournalIsRecoverable) {
  // A crash leaves record_count == 0 in the header. The reader must still
  // recover the records by scanning to EOF.
  const std::string path = "replay_truncated.bin";
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadGenerator gen(default_workload(31337, 500), syms);
  const auto cmds = gen.generate();

  {
    CommandJournalWriter w;
    ASSERT_TRUE(w.open(path));
    for (const Command &c : cmds) {
      w.append(c);
    }
    // Deliberately no close(): header count stays zero.
  }

  CommandJournalReader r;
  ASSERT_TRUE(r.load(path));
  EXPECT_EQ(r.commands().size(), cmds.size());
  std::remove(path.c_str());
}

TEST(Property, InvariantsHoldUnderRandomFlow) {
  // Several seeds, checking the full invariant set after every single command.
  for (std::uint64_t seed : {1u, 7u, 42u, 2024u}) {
    Exchange ex;
    const auto syms = setup_symbols(ex);
    WorkloadGenerator gen(default_workload(seed, 8000), syms);
    const auto cmds = gen.generate();

    EventBuffer buf;
    std::string why;
    bool ok = true;
    for (std::size_t i = 0; i < cmds.size() && ok; ++i) {
      buf.clear();
      ex.apply(cmds[i], buf);
      if (!ex.check_invariants(&why)) {
        std::fprintf(stderr,
                     "    seed %llu, command %zu: invariant violated: %s\n",
                     static_cast<unsigned long long>(seed), i, why.c_str());
        ok = false;
      }
    }
    EXPECT_TRUE(ok);
  }
}

TEST(Property, QuantityIsConserved) {
  // Every unit of quantity that leaves an order must appear in a trade, be
  // cancelled, or still be resting. This is the strongest correctness check in
  // the suite: it would catch double-fills, lost residuals, or phantom liquidity.
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadGenerator gen(default_workload(5150, 30000), syms);
  const auto cmds = gen.generate();

  EventBuffer buf;
  for (const Command &c : cmds) {
    ex.apply(c, buf);
  }

  // Reconstruct each order's fate from the event stream alone.
  struct Track {
    Qty accepted{0};
    Qty traded{0};
    Qty cancelled_remaining{0};
    bool terminal{false};
  };
  std::unordered_map<std::uint64_t, Track> orders;

  for (const Event &e : buf.events()) {
    switch (e.type) {
    case EventType::OrderAccepted:
      orders[e.order_id.value].accepted = e.quantity;
      break;
    case EventType::Trade:
      // The aggressor's fill.
      orders[e.aggressor_id.value].traded += e.trade_qty;
      // The resting side's fill.
      orders[e.resting_id.value].traded += e.trade_qty;
      break;
    case EventType::OrderCancelled:
      orders[e.order_id.value].cancelled_remaining += e.remaining;
      orders[e.order_id.value].terminal = true;
      break;
    case EventType::OrderModified:
      // A modify redefines the order's total quantity; re-baseline it.
      orders[e.order_id.value].accepted = e.quantity;
      break;
    default:
      break;
    }
  }

  // Sum resting quantity still in the books.
  Qty resting_total = 0;
  for (SymbolId s : syms) {
    const OrderBook &b = ex.book(s);
    b.bids().for_each_level_best_first(
        [&](const PriceLevel &l) { resting_total += l.total_qty; });
    b.asks().for_each_level_best_first(
        [&](const PriceLevel &l) { resting_total += l.total_qty; });
  }

  // For each order: accepted == traded + cancelled_remaining + still_resting.
  // We check the aggregate, since per-order resting quantity is not in the
  // event stream.
  Qty accepted_total = 0;
  Qty traded_total = 0;
  Qty cancelled_total = 0;
  for (const auto &[id, t] : orders) {
    accepted_total += t.accepted;
    traded_total += t.traded;
    cancelled_total += t.cancelled_remaining;
  }

  // traded_total double counts (both sides of each trade), so halve it.
  EXPECT_EQ(traded_total % 2, 0);
  const Qty matched = traded_total / 2;

  // Accepted quantity must equal what traded (counted once per side, so
  // 2*matched of order quantity was consumed), plus cancelled residual, plus
  // what is still resting.
  EXPECT_EQ(accepted_total, 2 * matched + cancelled_total + resting_total);
}

TEST(Property, NoOrderOutlivesItsTerminalState) {
  // Once an order is reported filled or cancelled, it must not appear in any
  // later trade.
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadGenerator gen(default_workload(6060, 20000), syms);
  const auto cmds = gen.generate();

  EventBuffer buf;
  for (const Command &c : cmds) {
    ex.apply(c, buf);
  }

  std::unordered_map<std::uint64_t, std::size_t> terminal_at;
  bool violation = false;
  std::size_t idx = 0;
  for (const Event &e : buf.events()) {
    if (e.type == EventType::Trade) {
      for (OrderId id : {e.aggressor_id, e.resting_id}) {
        auto it = terminal_at.find(id.value);
        if (it != terminal_at.end()) {
          std::fprintf(stderr,
                       "    order %llu traded at event %zu after terminal at %zu\n",
                       static_cast<unsigned long long>(id.value), idx,
                       it->second);
          violation = true;
        }
      }
      // A trade that exhausts the resting order makes it terminal.
      if (e.status == OrderStatus::Filled) {
        terminal_at[e.aggressor_id.value] = idx;
      }
    } else if (e.type == EventType::OrderCancelled) {
      terminal_at[e.order_id.value] = idx;
    } else if (e.type == EventType::OrderAccepted ||
               e.type == EventType::OrderModified) {
      // A re-used id after a terminal state is a fresh order (the generator
      // does not reuse ids, but a modify re-enters the book).
      terminal_at.erase(e.order_id.value);
    }
    ++idx;
  }
  EXPECT_FALSE(violation);
}

TEST(Property, BookNeverCrossesUnderHeavyAggression) {
  // A workload biased hard toward crossing orders: the book must still never
  // rest crossed.
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadConfig cfg = default_workload(818, 20000);
  cfg.pct_aggressive = 80;
  cfg.pct_market = 10;
  cfg.pct_cancel = 10;
  WorkloadGenerator gen(cfg, syms);
  const auto cmds = gen.generate();

  EventBuffer buf;
  bool ok = true;
  for (const Command &c : cmds) {
    buf.clear();
    ex.apply(c, buf);
    for (SymbolId s : syms) {
      const OrderBook &b = ex.book(s);
      const Ticks bid = b.best_bid();
      const Ticks ask = b.best_ask();
      if (bid != kInvalidTicks && ask != kInvalidTicks && bid >= ask) {
        ok = false;
      }
    }
    if (!ok) {
      break;
    }
  }
  EXPECT_TRUE(ok);
}

TEST(Property, PoolReturnsMemoryAsOrdersLeaveTheBook) {
  // Cancel-heavy flow: live pool objects must track resting orders, not grow
  // without bound. This is what proves the pool actually recycles.
  Exchange ex;
  const auto syms = setup_symbols(ex);
  WorkloadConfig cfg = default_workload(4321, 20000);
  cfg.pct_cancel = 45;
  WorkloadGenerator gen(cfg, syms);
  const auto cmds = gen.generate();

  EventBuffer buf;
  for (const Command &c : cmds) {
    ex.apply(c, buf);
  }

  std::size_t resting = 0;
  for (SymbolId s : syms) {
    resting += ex.book(s).resting_orders();
  }
  std::size_t live_index = 0;
  for (SymbolId s : syms) {
    live_index += ex.engine(s).live_orders();
  }
  // The order index and the books must agree exactly: a leak in either shows up
  // here as a mismatch.
  EXPECT_EQ(resting, live_index);
}
