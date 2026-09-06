// Shared test scaffolding: a single-symbol exchange plus helpers that keep the
// tests readable (submit an order, count trades, read the book).
#pragma once

#include "TestHarness.hpp"

#include "exchange/Exchange.hpp"

#include <string>
#include <vector>

namespace te::test {

// Client ids used across the suite. Distinct clients by default so that
// self-match prevention only triggers where a test intends it.
inline constexpr ClientId kAlice{101};
inline constexpr ClientId kBob{202};
inline constexpr ClientId kCarol{303};

class ExchangeFixture : public ::testing::Test {
public:
  void SetUp() override {
    ExchangeConfig cfg;
    cfg.prealloc_orders = 4096;
    ex = std::make_unique<Exchange>(cfg);
    // tick_size 1, price_scale 100 => prices are integer cents.
    sym = ex->add_instrument("AAPL", 1, 100, 1);
    sym2 = ex->add_instrument("MSFT", 1, 100, 1);
    next_id = 1;
  }

  void TearDown() override {
    std::string why;
    // Every test ends with a book-integrity check, so a semantic bug shows up
    // even in a test that was not written to look for it.
    EXPECT_TRUE(ex->check_invariants(&why)) ;
    if (!why.empty()) {
      std::fprintf(stderr, "    invariant violation: %s\n", why.c_str());
    }
  }

  // --- submission helpers ------------------------------------------------
  OrderId limit(Side side, Ticks price, Qty qty, ClientId client = kAlice,
                TimeInForce tif = TimeInForce::Day, SymbolId s = SymbolId{0}) {
    const OrderId id{next_id++};
    buf.clear();
    ex->apply(Command::new_limit(s.valid() ? s : sym, client, id, side, price,
                                 qty, tif),
              buf);
    return id;
  }

  OrderId market(Side side, Qty qty, ClientId client = kAlice,
                 SymbolId s = SymbolId{0}) {
    const OrderId id{next_id++};
    buf.clear();
    ex->apply(Command::new_market(s.valid() ? s : sym, client, id, side, qty),
              buf);
    return id;
  }

  void cancel(OrderId id, ClientId client = kAlice, SymbolId s = SymbolId{0}) {
    buf.clear();
    ex->apply(Command::cancel(s.valid() ? s : sym, client, id), buf);
  }

  void modify(OrderId id, Ticks price, Qty qty, ClientId client = kAlice,
              SymbolId s = SymbolId{0}) {
    buf.clear();
    ex->apply(Command::modify(s.valid() ? s : sym, client, id, price, qty), buf);
  }

  // --- inspection helpers -----------------------------------------------
  [[nodiscard]] const OrderBook &book(SymbolId s = SymbolId{0}) const {
    return ex->book(s.valid() ? s : sym);
  }
  [[nodiscard]] Ticks best_bid() const { return book().best_bid(); }
  [[nodiscard]] Ticks best_ask() const { return book().best_ask(); }
  [[nodiscard]] Qty bid_qty() const { return book().best_bid_qty(); }
  [[nodiscard]] Qty ask_qty() const { return book().best_ask_qty(); }

  // Events from the most recent command only.
  [[nodiscard]] std::size_t trades() const { return buf.count(EventType::Trade); }
  [[nodiscard]] std::size_t rejects() const {
    return buf.count(EventType::OrderRejected);
  }
  [[nodiscard]] std::size_t cancels() const {
    return buf.count(EventType::OrderCancelled);
  }
  [[nodiscard]] RejectReason reject_reason() const {
    const Event *e = buf.first(EventType::OrderRejected);
    return e != nullptr ? e->reject : RejectReason::None;
  }
  [[nodiscard]] std::vector<Event> trade_events() const {
    std::vector<Event> out;
    for (const Event &e : buf.events()) {
      if (e.type == EventType::Trade) {
        out.push_back(e);
      }
    }
    return out;
  }
  [[nodiscard]] Qty traded_qty() const {
    Qty q = 0;
    for (const Event &e : buf.events()) {
      if (e.type == EventType::Trade) {
        q += e.trade_qty;
      }
    }
    return q;
  }
  [[nodiscard]] bool order_live(OrderId id) const {
    OrderView v;
    return ex->lookup_order(id, v);
  }
  [[nodiscard]] OrderView view(OrderId id) const {
    OrderView v;
    (void)ex->lookup_order(id, v); // default-constructed view when unknown
    return v;
  }

  std::unique_ptr<Exchange> ex;
  SymbolId sym{};
  SymbolId sym2{};
  EventBuffer buf;
  std::uint64_t next_id{1};
};

} // namespace te::test
