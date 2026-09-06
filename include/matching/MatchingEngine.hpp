// Single-instrument matching engine.
//
// ---------------------------------------------------------------------------
// Semantics implemented here (the parts a reviewer will ask about)
// ---------------------------------------------------------------------------
// Priority
//   Strict price-time. The aggressor sweeps the opposite side best-price-first,
//   and within a price level it consumes the FIFO front first. Time priority is
//   ordered by exchange sequence number, not wall clock, so it is deterministic
//   under replay.
//
// Trade price
//   Trades print at the *resting* order's price, never the aggressor's. The
//   resting order set the terms; an aggressor willing to pay more receives the
//   price improvement. This is standard venue behaviour and it is also what
//   makes a crossed book impossible after a sweep.
//
// Market orders
//   Never rest. They sweep available liquidity and the residual is cancelled
//   (CancelCause::IocResidual). A market order into an empty book is rejected
//   with NoLiquidity rather than silently accepted-and-cancelled, because a
//   client needs to distinguish "no liquidity existed" from "partially filled".
//
// Time in force
//   Day rests the residual. Ioc cancels the residual. Fok is evaluated before
//   any state change: the engine first computes the fillable quantity at the
//   order's limit (honouring self-match exclusions) and rejects the whole order
//   if it cannot be filled completely. Fok therefore never produces a partial
//   fill or a visible intermediate state.
//
// Self-match prevention (policy: CANCEL_RESTING)
//   When an aggressor would trade against a resting order from the same
//   ClientId, the resting order is cancelled and the aggressor continues
//   sweeping. Chosen over cancelling the aggressor because it is the least
//   surprising for the common cause of self-matching — a stale quote left by
//   the same participant — and it preserves the intent of the newer order.
//   Cancelling the aggressor instead would let one forgotten resting order
//   block a participant from trading at that price indefinitely. The trade-off
//   is that a participant can remove their own resting liquidity by crossing
//   it, so the cancel is reported explicitly with
//   CancelCause::SelfMatchPrevention.
//
// Modify
//   Price change, or quantity *increase*, loses time priority: the order is
//   removed and re-inserted at the back of the new level with a fresh sequence
//   number, then re-matched (it may now cross). Quantity *decrease* at the same
//   price retains priority, adjusting the level total in place. This mirrors
//   real venue cancel/replace rules — priority is a scarce resource and must
//   not be obtainable by amending an old order upward.
//   A modify whose quantity decrease would take remaining to <= 0 is rejected
//   as InvalidModify; clients cancel to remove an order.
//
// Hot path discipline
//   No logging, no I/O, no exceptions, no allocation except pooled Order slots
//   and the amortised growth of the caller's EventBuffer.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Commands.hpp"
#include "core/Instrument.hpp"
#include "core/Order.hpp"
#include "core/Types.hpp"
#include "events/Events.hpp"
#include "orderbook/OrderBook.hpp"
#include "risk/RiskChecks.hpp"
#include "util/FlatHashMap.hpp"

#include <cstdint>

namespace te {

// Monotonic counters shared across the instruments of one matching shard, so
// that sequence numbers and trade ids are unique and ordered exchange-wide
// within that shard (a shard is single-threaded, so no atomics are needed).
struct SequenceSource {
  SeqNum next_seq{1};
  std::uint64_t next_trade{1};

  SeqNum seq() noexcept { return next_seq++; }
  TradeId trade() noexcept { return TradeId{next_trade++}; }
};

// Per-instrument statistics maintained for market data and the REST API.
struct InstrumentStats {
  Ticks last_price{kInvalidTicks};
  Qty last_qty{0};
  std::uint64_t trade_count{0};
  Qty volume{0};
  std::int64_t turnover_ticks{0}; // sum(price*qty), for VWAP
  std::uint64_t orders_accepted{0};
  std::uint64_t orders_rejected{0};
  std::uint64_t orders_cancelled{0};
  std::uint64_t orders_modified{0};

  [[nodiscard]] double vwap_ticks() const noexcept {
    return volume > 0 ? static_cast<double>(turnover_ticks) /
                            static_cast<double>(volume)
                      : 0.0;
  }
};

class MatchingEngine {
public:
  // `index_reserve` sizes the order index up front so the hot path does not
  // allocate. Bigger is not better: the table is open-addressed, so an
  // oversized table spreads lookups across more cache lines and more TLB
  // entries than the live order count needs. Measured on the mixed-flow
  // benchmark, raising this from 4096 to 65536 slowed the engine by ~85%
  // (127 -> 237 ns/command) purely through cache behaviour. Size it to the
  // expected number of simultaneously resting orders for the instrument, not
  // to the worst case; the map still grows correctly if exceeded.
  MatchingEngine(const Instrument &instrument, BookPools &pools,
                 SequenceSource &seqs, RiskLimits limits = {},
                 std::size_t index_reserve = 4096);

  // Applies one command, appending resulting events to `out`. The engine's
  // entire public surface for mutation is this single method: one command in, a
  // deterministic event sequence out.
  void apply(const Command &cmd, EventBuffer &out);

  [[nodiscard]] const OrderBook &book() const noexcept { return book_; }
  [[nodiscard]] OrderBook &book() noexcept { return book_; }
  [[nodiscard]] const InstrumentStats &stats() const noexcept { return stats_; }
  [[nodiscard]] const Instrument &instrument() const noexcept {
    return book_.instrument();
  }

  // Order lookup for query paths. Returns false when the id is unknown (never
  // resting, or already terminal and reaped).
  [[nodiscard]] bool lookup(OrderId id, OrderView &out) const;

  [[nodiscard]] std::size_t live_orders() const noexcept { return index_.size(); }

  // Timestamp source. Injectable so that benchmarks and deterministic replay can
  // supply a counter instead of reading the clock (clock_gettime is otherwise a
  // measurable fraction of per-order cost).
  using Clock = Nanos (*)();
  void set_clock(Clock c) noexcept { clock_ = c; }

private:
  // --- command handlers -------------------------------------------------
  void handle_new(const Command &cmd, EventBuffer &out);
  void handle_cancel(const Command &cmd, EventBuffer &out);
  void handle_modify(const Command &cmd, EventBuffer &out);

  // Sweeps the opposite side for `aggressor`, emitting trades and reducing or
  // removing resting orders. Returns when the aggressor is exhausted or no
  // eligible price remains. Applies self-match prevention.
  void match(Order &aggressor, EventBuffer &out);

  // Fillable quantity at `limit` for a FOK check, excluding the client's own
  // resting orders (which SMP would cancel rather than trade).
  [[nodiscard]] Qty fillable(Side side, Ticks limit, ClientId client,
                             OrderType type) const;

  // Rests the residual and registers it in the order index.
  void rest_order(const Order &proto, EventBuffer &out);

  // Removes a resting order from the book and the index.
  void remove_resting(Order &order);

  // --- event emission ---------------------------------------------------
  void emit_accepted(const Order &o, EventBuffer &out);
  void emit_rejected(const Command &cmd, RejectReason reason, EventBuffer &out);
  void emit_cancelled(const Order &o, CancelCause cause, EventBuffer &out);
  // Takes the resting order's identity by value: the resting node may already
  // have been returned to the pool by the time the event is emitted.
  void emit_trade(const Order &aggressor, OrderId resting_id,
                  ClientId resting_client, Ticks price, Qty qty,
                  EventBuffer &out);
  void emit_book_update(EventBuffer &out);
  void stamp_top_of_book(Event &e) const;

  [[nodiscard]] Nanos now() const noexcept { return clock_ != nullptr ? clock_() : 0; }

  OrderBook book_;
  SequenceSource *seqs_;
  RiskLimits limits_;
  InstrumentStats stats_;
  Clock clock_{nullptr};

  // OrderId -> resting node. Holds only live orders; entries are erased on fill
  // or cancel so the map does not grow without bound over a session.
  //
  // A hash map is the right structure here despite the pointer chase: cancels
  // arrive by id with no locality, and the alternative (scanning levels) is
  // orders of magnitude worse. Duplicate-id rejection also needs this lookup.
  //
  // FlatHashMap rather than std::unordered_map because the latter allocates a
  // node per insert. That showed up in the insert benchmark as a latency spike
  // on an exact 128-order period — the periodic minor page fault as malloc took
  // a fresh page for 32-byte hash nodes. See FlatHashMap.hpp.
  FlatHashMap<OrderId, Order *> index_;
};

} // namespace te
