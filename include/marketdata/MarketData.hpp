// Market data projection.
//
// The engine publishes events; it does not know about subscribers, sockets, or
// serialisation. This header defines the *derived* view a consumer wants (top of
// book, depth, last trade, session statistics) and a small subscriber interface
// the gateway implements.
//
// Keeping this out of the engine is what lets the same event stream drive the
// WebSocket publisher, the REST snapshot endpoint, and the replay verifier
// without any of them being coupled to matching internals.
#pragma once

#include "core/Instrument.hpp"
#include "core/Types.hpp"
#include "events/Events.hpp"
#include "orderbook/OrderBook.hpp"

#include <cstdint>
#include <vector>

namespace te {

// Best bid/offer.
struct TopOfBook {
  SymbolId symbol{};
  Ticks bid{kInvalidTicks};
  Qty bid_qty{0};
  Ticks ask{kInvalidTicks};
  Qty ask_qty{0};

  [[nodiscard]] bool has_bid() const noexcept { return bid != kInvalidTicks; }
  [[nodiscard]] bool has_ask() const noexcept { return ask != kInvalidTicks; }

  // Spread in ticks; kInvalidTicks when one-sided.
  [[nodiscard]] Ticks spread() const noexcept {
    return (has_bid() && has_ask()) ? ask - bid : kInvalidTicks;
  }

  // Mid in ticks, rounded down. Only meaningful when two-sided.
  [[nodiscard]] Ticks mid() const noexcept {
    return (has_bid() && has_ask()) ? (bid + ask) / 2 : kInvalidTicks;
  }
};

// Aggregated snapshot served to REST clients and the dashboard on subscribe.
struct BookSnapshot {
  SymbolId symbol{};
  SeqNum seq{0}; // sequence of the last event applied
  TopOfBook top;
  std::vector<DepthEntry> bids; // best first
  std::vector<DepthEntry> asks; // best first
  Ticks last_price{kInvalidTicks};
  Qty last_qty{0};
  Qty volume{0};
  std::uint64_t trade_count{0};
};

// Consumers implement this. Called synchronously from the publisher after the
// engine has finished a command, never from inside matching.
class MarketDataSubscriber {
public:
  virtual ~MarketDataSubscriber() = default;
  virtual void on_event(const Event &e) = 0;
};

// Fans engine events out to subscribers and maintains derived per-symbol state.
//
// Deliberately not a template or a signal/slot framework: a vector of raw
// observer pointers is enough, and it keeps the publish path a straight loop.
// Ownership of subscribers stays with the caller (the gateway), which outlives
// the publisher.
class MarketDataPublisher {
public:
  explicit MarketDataPublisher(const InstrumentTable &instruments);

  void subscribe(MarketDataSubscriber *sub);
  void unsubscribe(MarketDataSubscriber *sub);

  // Applies a batch of engine events: updates derived state, then fans out.
  void publish(const std::vector<Event> &events);
  void publish(const Event &event);

  [[nodiscard]] const TopOfBook &top_of_book(SymbolId sym) const {
    return tops_[sym.value];
  }
  [[nodiscard]] Ticks last_price(SymbolId sym) const { return last_price_[sym.value]; }
  [[nodiscard]] Qty last_qty(SymbolId sym) const { return last_qty_[sym.value]; }
  [[nodiscard]] SeqNum last_seq() const noexcept { return last_seq_; }

  // Recent trades, newest last, capped to a ring of `capacity` per symbol. The
  // dashboard's "recent trades" panel reads this instead of replaying the log.
  [[nodiscard]] std::vector<Event> recent_trades(SymbolId sym,
                                                 std::size_t max_n) const;

  void set_trade_history_capacity(std::size_t n) { trade_capacity_ = n; }

private:
  const InstrumentTable *instruments_;
  std::vector<MarketDataSubscriber *> subscribers_;
  std::vector<TopOfBook> tops_;
  std::vector<Ticks> last_price_;
  std::vector<Qty> last_qty_;
  std::vector<std::vector<Event>> trades_; // per symbol, bounded
  std::size_t trade_capacity_{256};
  SeqNum last_seq_{0};
};

} // namespace te
