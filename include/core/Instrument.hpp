// Instrument (symbol) definition and the instrument table.
//
// The engine addresses instruments by a dense SymbolId (an index), never by
// string. String lookup happens once, at the gateway boundary. This keeps the
// matching path free of hashing and string comparison, and lets the exchange
// hold a flat vector of order books indexed directly by symbol.
#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace te {

// Per-instrument static configuration and risk limits.
struct Instrument {
  SymbolId id{};
  std::string symbol;             // e.g. "AAPL"
  Ticks tick_size{1};             // minimum price increment, in ticks
  std::int64_t price_scale{100};  // ticks per unit of currency (100 => cents)
  Qty lot_size{1};                // quantity must be a multiple of this
  Qty max_order_qty{1'000'000};   // per-order quantity cap
  std::int64_t max_notional_ticks{1'000'000'000'000LL}; // price*qty cap

  [[nodiscard]] double to_price(Ticks t) const noexcept {
    return static_cast<double>(t) / static_cast<double>(price_scale);
  }

  [[nodiscard]] Ticks from_price(double p) const noexcept {
    // Round half away from zero so a UI-entered 101.005 does not silently
    // truncate to a different tick than the user saw.
    const double scaled = p * static_cast<double>(price_scale);
    return static_cast<Ticks>(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
  }

  [[nodiscard]] bool on_tick(Ticks t) const noexcept {
    return tick_size <= 1 || (t % tick_size) == 0;
  }
};

// Owns the instrument definitions. Built at startup and then read-only, which
// is what allows the matching threads to share it without synchronisation.
//
// Storage is a std::deque, not a std::vector: every MatchingEngine holds a
// `const Instrument&` for the lifetime of the exchange, and a vector would
// reallocate on the next add_instrument() and dangle all of them. A deque
// guarantees reference stability across push_back. (AddressSanitizer caught
// exactly this — the tests passed regardless, because the freed memory still
// held plausible values.)
class InstrumentTable {
public:
  // Returns the id of the newly added instrument. Duplicate symbols return the
  // existing id so repeated configuration loading is idempotent.
  SymbolId add(const std::string &symbol, Ticks tick_size = 1,
               std::int64_t price_scale = 100, Qty lot_size = 1,
               Qty max_order_qty = 1'000'000,
               std::int64_t max_notional_ticks = 1'000'000'000'000LL);

  [[nodiscard]] bool contains(SymbolId id) const noexcept {
    return id.value < instruments_.size();
  }

  // Precondition: contains(id). Hot path: no bounds check beyond the assert in
  // debug builds, callers validate once at the gateway.
  [[nodiscard]] const Instrument &get(SymbolId id) const noexcept {
    return instruments_[id.value];
  }

  // String -> id, for the gateway/config edge only. Returns an invalid id when
  // the symbol is unknown; callers must check .valid().
  [[nodiscard]] SymbolId find(const std::string &symbol) const;

  [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

  [[nodiscard]] const std::deque<Instrument> &all() const noexcept {
    return instruments_;
  }

private:
  // Index 0 is a reserved sentinel so that a default-constructed SymbolId{0}
  // is never a valid tradable instrument.
  std::deque<Instrument> instruments_{Instrument{}};
  std::unordered_map<std::string, SymbolId> by_symbol_;
};

} // namespace te
