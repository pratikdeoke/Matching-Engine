// Outbound event model.
//
// The engine never prints, logs, or calls a socket. It appends fixed-size PODs
// to an EventBuffer that the caller drains. That gives us three things at once:
//   * the hot path does no I/O and no allocation;
//   * tests assert on structured events instead of parsing stdout;
//   * the same event stream feeds market data, the event log, and replay
//     verification without duplicating engine logic.
//
// One tagged struct is used instead of a class hierarchy with virtuals: events
// are copied and scanned in bulk, so a flat trivially-copyable type is both
// faster and directly writable to a binary log.
#pragma once

#include "core/Order.hpp"
#include "core/Types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace te {

enum class EventType : std::uint8_t {
  OrderAccepted = 0,
  OrderRejected,
  OrderCancelled,
  OrderModified,
  Trade,
  // Emitted once per command after any book mutation, carrying the resulting
  // top of book. Consumers that only care about quotes can filter on this and
  // ignore order-level traffic.
  BookUpdate,
};

[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
  switch (t) {
  case EventType::OrderAccepted:
    return "ORDER_ACCEPTED";
  case EventType::OrderRejected:
    return "ORDER_REJECTED";
  case EventType::OrderCancelled:
    return "ORDER_CANCELLED";
  case EventType::OrderModified:
    return "ORDER_MODIFIED";
  case EventType::Trade:
    return "TRADE";
  case EventType::BookUpdate:
    return "BOOK_UPDATE";
  }
  return "?";
}

// Why an order left the book. Distinguishing client cancels from engine-driven
// removals (IOC residual, self-match policy) matters to a client's OMS.
enum class CancelCause : std::uint8_t {
  ClientRequest = 0,
  IocResidual,
  FokUnfillable,
  SelfMatchPrevention,
  Replaced, // removed as part of a modify that lost priority
};

[[nodiscard]] constexpr std::string_view to_string(CancelCause c) noexcept {
  switch (c) {
  case CancelCause::ClientRequest:
    return "CLIENT_REQUEST";
  case CancelCause::IocResidual:
    return "IOC_RESIDUAL";
  case CancelCause::FokUnfillable:
    return "FOK_UNFILLABLE";
  case CancelCause::SelfMatchPrevention:
    return "SELF_MATCH_PREVENTION";
  case CancelCause::Replaced:
    return "REPLACED";
  }
  return "?";
}

struct Event {
  EventType type{EventType::OrderAccepted};
  SeqNum seq{0};    // exchange-wide event sequence, gapless and monotonic
  Nanos ts{0};      // engine timestamp
  SymbolId symbol{};

  // --- order-scoped fields (Accepted / Rejected / Cancelled / Modified) ---
  OrderId order_id{};
  ClientId client{};
  Side side{Side::Buy};
  OrderType order_type{OrderType::Limit};
  OrderStatus status{OrderStatus::New};
  Ticks price{0};
  Qty quantity{0};  // order quantity (post-modify quantity for Modified)
  Qty remaining{0}; // residual after the command was applied
  RejectReason reject{RejectReason::None};
  CancelCause cancel_cause{CancelCause::ClientRequest};

  // --- trade fields -----------------------------------------------------
  TradeId trade_id{};
  Ticks trade_price{0};    // always the *resting* order's price
  Qty trade_qty{0};
  OrderId aggressor_id{};  // liquidity taker
  OrderId resting_id{};    // liquidity provider
  ClientId aggressor_client{};
  ClientId resting_client{};
  Aggressor aggressor_side{Aggressor::Buy};

  // --- top of book (BookUpdate, also filled on Trade for convenience) ----
  Ticks best_bid{kInvalidTicks};
  Qty best_bid_qty{0};
  Ticks best_ask{kInvalidTicks};
  Qty best_ask_qty{0};

  // --- modify bookkeeping ----------------------------------------------
  Ticks old_price{0};
  Qty old_quantity{0};
  bool priority_retained{false};
};

static_assert(sizeof(Event) <= 256, "Event should stay small enough to copy cheaply");

// Growable sink for engine output. The engine only ever push_back()s; the caller
// drains and clears. Reusing one buffer across commands keeps the capacity warm
// so steady-state event emission does not allocate.
class EventBuffer {
public:
  explicit EventBuffer(std::size_t reserve_n = 1024) {
    events_.reserve(reserve_n);
  }

  void push(const Event &e) { events_.push_back(e); }

  [[nodiscard]] const std::vector<Event> &events() const noexcept {
    return events_;
  }
  [[nodiscard]] std::vector<Event> &events() noexcept { return events_; }

  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
  [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
  [[nodiscard]] const Event &operator[](std::size_t i) const { return events_[i]; }

  // Clears contents but keeps capacity: the point is to avoid re-allocating on
  // every command.
  void clear() noexcept { events_.clear(); }

  // Convenience for tests and tools.
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto &e : events_) {
      n += (e.type == t) ? 1 : 0;
    }
    return n;
  }

  [[nodiscard]] const Event *first(EventType t) const {
    for (const auto &e : events_) {
      if (e.type == t) {
        return &e;
      }
    }
    return nullptr;
  }

private:
  std::vector<Event> events_;
};

} // namespace te
