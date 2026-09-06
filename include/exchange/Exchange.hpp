// Multi-instrument exchange.
//
// Owns the instrument table, one MatchingEngine per instrument, and the shared
// sequence source. Commands are routed by SymbolId — a flat vector index, so
// routing is an array lookup rather than a hash of a symbol string.
//
// Threading: this class is single-threaded by design. One Exchange instance is a
// *shard*; the concurrency story is to run several shards, each owning a
// disjoint set of instruments, with one thread per shard (single-writer per
// book). That is why nothing here is atomic and why the sequence source is a
// plain counter. Sharding by symbol is the standard exchange architecture and it
// avoids the alternative of a global lock around matching, which would serialise
// the whole venue on its slowest instrument.
#pragma once

#include "core/Commands.hpp"
#include "core/Instrument.hpp"
#include "events/Events.hpp"
#include "marketdata/MarketData.hpp"
#include "matching/MatchingEngine.hpp"
#include "orderbook/OrderBook.hpp"
#include "risk/RiskChecks.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace te {

struct ExchangeConfig {
  RiskLimits limits{};
  std::size_t order_pool_chunk{8192};
  std::size_t level_pool_chunk{1024};
  // Pre-touch pool memory so a benchmark or session start does not pay for
  // first-fault page allocation in the middle of measurement.
  std::size_t prealloc_orders{0};
  // Per-instrument order-index capacity. See MatchingEngine's constructor: this
  // is a cache-footprint knob, and larger values measurably hurt.
  std::size_t order_index_reserve{4096};
};

class Exchange {
public:
  explicit Exchange(ExchangeConfig cfg = {});

  // --- setup (before trading) -------------------------------------------
  SymbolId add_instrument(const std::string &symbol, Ticks tick_size = 1,
                          std::int64_t price_scale = 100, Qty lot_size = 1,
                          Qty max_order_qty = 1'000'000,
                          std::int64_t max_notional_ticks = 1'000'000'000'000LL);

  [[nodiscard]] const InstrumentTable &instruments() const noexcept {
    return instruments_;
  }
  [[nodiscard]] SymbolId find_symbol(const std::string &s) const {
    return instruments_.find(s);
  }

  // --- trading ----------------------------------------------------------
  // Applies one command and appends its events to `out`. An unknown symbol is
  // rejected here rather than inside an engine, since no engine owns it.
  void apply(const Command &cmd, EventBuffer &out);

  // Convenience overload: appends to the exchange's internal buffer, which the
  // caller then drains via events(). Used by tools and the gateway.
  void apply(const Command &cmd);

  [[nodiscard]] EventBuffer &events() noexcept { return events_; }
  [[nodiscard]] const EventBuffer &events() const noexcept { return events_; }

  // --- queries ----------------------------------------------------------
  [[nodiscard]] MatchingEngine &engine(SymbolId s) { return *engines_[s.value]; }
  [[nodiscard]] const MatchingEngine &engine(SymbolId s) const {
    return *engines_[s.value];
  }
  [[nodiscard]] const OrderBook &book(SymbolId s) const {
    return engines_[s.value]->book();
  }
  [[nodiscard]] bool has_symbol(SymbolId s) const noexcept {
    return s.valid() && s.value < engines_.size() && engines_[s.value] != nullptr;
  }

  [[nodiscard]] BookSnapshot snapshot(SymbolId s, std::size_t depth = 10) const;

  // Locates an order without knowing its instrument. Linear in instrument count;
  // intended for admin/REST paths, not the hot path.
  [[nodiscard]] bool lookup_order(OrderId id, OrderView &out) const;

  [[nodiscard]] SeqNum last_seq() const noexcept { return seqs_.next_seq - 1; }

  // Verifies every book's invariants. Used by tests and the property harness.
  [[nodiscard]] bool check_invariants(std::string *why = nullptr) const;

  void set_clock(MatchingEngine::Clock c);

private:
  ExchangeConfig cfg_;
  InstrumentTable instruments_;
  BookPools pools_;
  SequenceSource seqs_;
  // unique_ptr because MatchingEngine holds an OrderBook, which holds BookSides
  // that are non-copyable and non-movable (they own pooled levels and are
  // referenced by intrusive order links). A vector of values would require
  // moving them on growth.
  std::vector<std::unique_ptr<MatchingEngine>> engines_;
  EventBuffer events_;
};

} // namespace te
