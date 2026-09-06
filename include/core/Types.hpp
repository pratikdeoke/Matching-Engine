// Core scalar/strong types for the exchange.
//
// Design decision: prices are represented as signed integer *ticks*, not doubles.
// A double price makes matching non-deterministic across compilers/platforms and
// forces epsilon comparisons into the hottest branch of the engine. Ticks give
// exact equality/ordering, cheap comparison, and a natural mapping to real
// exchange protocols (which are integer-based). Conversion to/from a human
// decimal price happens only at the edges (gateway / UI) via Instrument.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace te {

using Ticks = std::int64_t;   // price in instrument ticks
using Qty = std::int64_t;     // quantity in lots/shares
using SeqNum = std::uint64_t; // monotonic exchange sequence number
using Nanos = std::int64_t;   // steady-clock nanoseconds since engine epoch

inline constexpr Ticks kInvalidTicks = std::numeric_limits<Ticks>::min();
inline constexpr Ticks kMaxTicks = std::numeric_limits<Ticks>::max();

// ---------------------------------------------------------------------------
// Strongly typed identifiers.
//
// These are thin wrappers over uint64_t: zero runtime cost, but a ClientId can
// never be silently passed where an OrderId is expected. That mistake is easy to
// make in an order-management layer and expensive to debug.
// ---------------------------------------------------------------------------
#define TE_DEFINE_ID(Name)                                                     \
  struct Name {                                                                \
    std::uint64_t value{0};                                                    \
    constexpr Name() = default;                                                \
    constexpr explicit Name(std::uint64_t v) : value(v) {}                     \
    friend constexpr bool operator==(Name a, Name b) {                         \
      return a.value == b.value;                                              \
    }                                                                          \
    friend constexpr bool operator!=(Name a, Name b) {                         \
      return a.value != b.value;                                              \
    }                                                                          \
    friend constexpr bool operator<(Name a, Name b) {                          \
      return a.value < b.value;                                               \
    }                                                                          \
    [[nodiscard]] constexpr bool valid() const { return value != 0; }          \
  };

TE_DEFINE_ID(OrderId)  // exchange-assigned order identifier
TE_DEFINE_ID(ClientId) // trading participant / account
TE_DEFINE_ID(TradeId)  // execution identifier
TE_DEFINE_ID(SymbolId) // dense index into the instrument table
#undef TE_DEFINE_ID

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

enum class OrderType : std::uint8_t {
  Limit = 0,
  Market = 1,
};

[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
  switch (t) {
  case OrderType::Limit:
    return "LIMIT";
  case OrderType::Market:
    return "MARKET";
  }
  return "?";
}

// Day  : rest on the book until cancelled (the default resting behaviour).
// Ioc  : match what is available, cancel the residual.
// Fok  : match in full or reject entirely (no partial fill, nothing rests).
enum class TimeInForce : std::uint8_t {
  Day = 0,
  Ioc = 1,
  Fok = 2,
};

[[nodiscard]] constexpr std::string_view to_string(TimeInForce t) noexcept {
  switch (t) {
  case TimeInForce::Day:
    return "DAY";
  case TimeInForce::Ioc:
    return "IOC";
  case TimeInForce::Fok:
    return "FOK";
  }
  return "?";
}

enum class OrderStatus : std::uint8_t {
  New = 0,
  Accepted = 1,
  PartiallyFilled = 2,
  Filled = 3,
  Cancelled = 4,
  Rejected = 5,
};

[[nodiscard]] constexpr std::string_view to_string(OrderStatus s) noexcept {
  switch (s) {
  case OrderStatus::New:
    return "NEW";
  case OrderStatus::Accepted:
    return "ACCEPTED";
  case OrderStatus::PartiallyFilled:
    return "PARTIALLY_FILLED";
  case OrderStatus::Filled:
    return "FILLED";
  case OrderStatus::Cancelled:
    return "CANCELLED";
  case OrderStatus::Rejected:
    return "REJECTED";
  }
  return "?";
}

[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
  return s == OrderStatus::Filled || s == OrderStatus::Cancelled ||
         s == OrderStatus::Rejected;
}

// Why an order/command was rejected. Returned in a Result rather than thrown:
// rejects are ordinary control flow at an exchange, not exceptional conditions,
// and we do not want unwinding machinery anywhere near the matching path.
enum class RejectReason : std::uint8_t {
  None = 0,
  UnknownSymbol,
  InvalidQuantity,
  InvalidPrice,
  PriceNotOnTick,
  QuantityAboveLimit,
  NotionalAboveLimit,
  DuplicateOrderId,
  UnknownOrderId,
  NotOrderOwner,
  OrderNotActive,
  MarketOrderNotAllowedToRest,
  NoLiquidity,
  FillOrKillUnfillable,
  SelfMatchPrevented,
  BookCapacityExceeded,
  InvalidModify,
};

[[nodiscard]] constexpr std::string_view to_string(RejectReason r) noexcept {
  switch (r) {
  case RejectReason::None:
    return "NONE";
  case RejectReason::UnknownSymbol:
    return "UNKNOWN_SYMBOL";
  case RejectReason::InvalidQuantity:
    return "INVALID_QUANTITY";
  case RejectReason::InvalidPrice:
    return "INVALID_PRICE";
  case RejectReason::PriceNotOnTick:
    return "PRICE_NOT_ON_TICK";
  case RejectReason::QuantityAboveLimit:
    return "QUANTITY_ABOVE_LIMIT";
  case RejectReason::NotionalAboveLimit:
    return "NOTIONAL_ABOVE_LIMIT";
  case RejectReason::DuplicateOrderId:
    return "DUPLICATE_ORDER_ID";
  case RejectReason::UnknownOrderId:
    return "UNKNOWN_ORDER_ID";
  case RejectReason::NotOrderOwner:
    return "NOT_ORDER_OWNER";
  case RejectReason::OrderNotActive:
    return "ORDER_NOT_ACTIVE";
  case RejectReason::MarketOrderNotAllowedToRest:
    return "MARKET_ORDER_CANNOT_REST";
  case RejectReason::NoLiquidity:
    return "NO_LIQUIDITY";
  case RejectReason::FillOrKillUnfillable:
    return "FOK_UNFILLABLE";
  case RejectReason::SelfMatchPrevented:
    return "SELF_MATCH_PREVENTED";
  case RejectReason::BookCapacityExceeded:
    return "BOOK_CAPACITY_EXCEEDED";
  case RejectReason::InvalidModify:
    return "INVALID_MODIFY";
  }
  return "?";
}

// Which side initiated the trade (took liquidity). Needed by market data
// consumers to infer trade direction without seeing the book.
enum class Aggressor : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Aggressor aggressor_of(Side s) noexcept {
  return s == Side::Buy ? Aggressor::Buy : Aggressor::Sell;
}

} // namespace te

// std::hash specialisations so the strong ids can key unordered containers.
#define TE_HASH_ID(Name)                                                       \
  template <> struct std::hash<te::Name> {                                     \
    std::size_t operator()(te::Name id) const noexcept {                       \
      /* ids are dense counters; splitmix64 avoids clustering in open          \
         addressing / bucket reuse. */                                         \
      std::uint64_t x = id.value + 0x9e3779b97f4a7c15ULL;                      \
      x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;                             \
      x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;                             \
      return static_cast<std::size_t>(x ^ (x >> 31));                          \
    }                                                                          \
  };

TE_HASH_ID(OrderId)
TE_HASH_ID(ClientId)
TE_HASH_ID(TradeId)
TE_HASH_ID(SymbolId)
#undef TE_HASH_ID
