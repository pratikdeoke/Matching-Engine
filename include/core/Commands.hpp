// Inbound command model.
//
// Commands are the engine's only input. They are POD, fixed size, and carry no
// strings — the symbol is already resolved to a SymbolId by the gateway. That
// makes the command stream trivially serialisable, which is what the event log
// and the replay tool depend on: replaying the same command sequence through a
// fresh engine must reproduce the same state and the same events.
#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace te {

enum class CommandType : std::uint8_t {
  NewOrder = 0,
  CancelOrder = 1,
  ModifyOrder = 2,
};

[[nodiscard]] constexpr std::string_view to_string(CommandType t) noexcept {
  switch (t) {
  case CommandType::NewOrder:
    return "NEW_ORDER";
  case CommandType::CancelOrder:
    return "CANCEL_ORDER";
  case CommandType::ModifyOrder:
    return "MODIFY_ORDER";
  }
  return "?";
}

struct Command {
  CommandType type{CommandType::NewOrder};
  SymbolId symbol{};
  ClientId client{};

  // Client-supplied order id. The engine uses client-supplied ids (rejecting
  // duplicates) rather than assigning its own, because it makes the command log
  // self-describing: a cancel in the log refers to the same id as the new order,
  // with no id-mapping table needed to replay.
  OrderId order_id{};

  // NewOrder / ModifyOrder fields.
  Side side{Side::Buy};
  OrderType order_type{OrderType::Limit};
  TimeInForce tif{TimeInForce::Day};
  Ticks price{0};
  Qty quantity{0};

  Nanos ts_received{0}; // gateway receive timestamp, telemetry only

  // --- factory helpers (keep call sites readable) ------------------------
  static Command new_limit(SymbolId sym, ClientId cl, OrderId oid, Side side,
                           Ticks price, Qty qty,
                           TimeInForce tif = TimeInForce::Day) {
    Command c;
    c.type = CommandType::NewOrder;
    c.symbol = sym;
    c.client = cl;
    c.order_id = oid;
    c.side = side;
    c.order_type = OrderType::Limit;
    c.tif = tif;
    c.price = price;
    c.quantity = qty;
    return c;
  }

  static Command new_market(SymbolId sym, ClientId cl, OrderId oid, Side side,
                            Qty qty) {
    Command c;
    c.type = CommandType::NewOrder;
    c.symbol = sym;
    c.client = cl;
    c.order_id = oid;
    c.side = side;
    c.order_type = OrderType::Market;
    // A market order never rests, so IOC is the only coherent TIF for it.
    c.tif = TimeInForce::Ioc;
    c.price = 0;
    c.quantity = qty;
    return c;
  }

  static Command cancel(SymbolId sym, ClientId cl, OrderId oid) {
    Command c;
    c.type = CommandType::CancelOrder;
    c.symbol = sym;
    c.client = cl;
    c.order_id = oid;
    return c;
  }

  static Command modify(SymbolId sym, ClientId cl, OrderId oid, Ticks new_price,
                        Qty new_qty) {
    Command c;
    c.type = CommandType::ModifyOrder;
    c.symbol = sym;
    c.client = cl;
    c.order_id = oid;
    c.price = new_price;
    c.quantity = new_qty;
    return c;
  }
};

static_assert(sizeof(Command) <= 128, "Command should stay compact");
// The journal writes Commands as raw bytes; trivial copyability is what makes
// that sound rather than merely convenient.
static_assert(std::is_trivially_copyable_v<Command>,
              "Command must be trivially copyable for journaling");

} // namespace te
