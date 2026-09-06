// Resting order representation.
//
// Design decision: Order is an *intrusive* node. It carries the prev/next links
// used by the price level's FIFO queue rather than being held in a separate
// std::list<Order> or shared_ptr. Consequences that matter:
//   * inserting/removing a resting order is pointer surgery, no allocation;
//   * cancel is O(1) given the node address, which is what the order index
//     stores, so cancel never scans a price level;
//   * one cache line holds everything matching needs.
#pragma once

#include "core/Types.hpp"

namespace te {

struct PriceLevel; // fwd

struct Order {
  // --- identity ---------------------------------------------------------
  OrderId id{};
  ClientId client{};
  SymbolId symbol{};

  // --- terms ------------------------------------------------------------
  Ticks price{0};   // limit price in ticks; unused for market orders
  Qty quantity{0};  // original quantity
  Qty remaining{0}; // unfilled quantity (== quantity until first fill)
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce tif{TimeInForce::Day};
  OrderStatus status{OrderStatus::New};

  // --- sequencing -------------------------------------------------------
  // seq is the exchange sequence number assigned on acceptance. It is the
  // tie-breaker for time priority: within a price level, lower seq is ahead.
  // Using a sequence rather than a wall-clock timestamp keeps ordering
  // deterministic under replay and immune to clock adjustments.
  SeqNum seq{0};
  Nanos ts_accepted{0}; // for latency/telemetry only, never for priority

  // --- intrusive book linkage ------------------------------------------
  Order *prev{nullptr};       // previous order at this price level (ahead in FIFO)
  Order *next{nullptr};       // next order at this price level
  PriceLevel *level{nullptr}; // owning level, null when not resting

  [[nodiscard]] Qty filled() const noexcept { return quantity - remaining; }

  [[nodiscard]] bool resting() const noexcept { return level != nullptr; }

  [[nodiscard]] bool active() const noexcept { return !is_terminal(status); }

  void reset_links() noexcept {
    prev = nullptr;
    next = nullptr;
    level = nullptr;
  }
};

// A snapshot of an order for query/reporting paths. Deliberately separate from
// Order so that handing order state to a REST endpoint or a test never exposes
// raw book pointers or lets a caller hold a dangling node.
struct OrderView {
  OrderId id{};
  ClientId client{};
  SymbolId symbol{};
  Ticks price{0};
  Qty quantity{0};
  Qty remaining{0};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce tif{TimeInForce::Day};
  OrderStatus status{OrderStatus::New};
  SeqNum seq{0};

  static OrderView of(const Order &o) noexcept {
    OrderView v;
    v.id = o.id;
    v.client = o.client;
    v.symbol = o.symbol;
    v.price = o.price;
    v.quantity = o.quantity;
    v.remaining = o.remaining;
    v.side = o.side;
    v.type = o.type;
    v.tif = o.tif;
    v.status = o.status;
    v.seq = o.seq;
    return v;
  }
};

} // namespace te
