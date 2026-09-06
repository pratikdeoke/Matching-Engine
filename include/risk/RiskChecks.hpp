// Pre-trade validation.
//
// Kept as a free function over (Instrument, Command) with no state so it can be
// unit-tested in isolation and inlined into the engine's accept path. Returns a
// RejectReason rather than throwing: a reject is a normal outcome and must not
// cost an unwind.
#pragma once

#include "core/Commands.hpp"
#include "core/Instrument.hpp"
#include "core/Types.hpp"

namespace te {

struct RiskLimits {
  // Global caps applied on top of per-instrument limits. Zero means "unlimited"
  // so a default-constructed RiskLimits is permissive.
  Qty max_order_qty{0};
  std::int64_t max_notional_ticks{0};
};

[[nodiscard]] inline RejectReason validate_new_order(const Instrument &inst,
                                                     const Command &cmd,
                                                     const RiskLimits &limits) {
  if (cmd.quantity <= 0) {
    return RejectReason::InvalidQuantity;
  }
  if (inst.lot_size > 1 && (cmd.quantity % inst.lot_size) != 0) {
    return RejectReason::InvalidQuantity;
  }
  if (cmd.quantity > inst.max_order_qty) {
    return RejectReason::QuantityAboveLimit;
  }
  if (limits.max_order_qty > 0 && cmd.quantity > limits.max_order_qty) {
    return RejectReason::QuantityAboveLimit;
  }

  if (cmd.order_type == OrderType::Limit) {
    if (cmd.price <= 0) {
      // Non-positive limit prices are rejected outright. Real venues do support
      // negative prices for some products; this engine trades equities, where a
      // non-positive limit is always an input error.
      return RejectReason::InvalidPrice;
    }
    if (!inst.on_tick(cmd.price)) {
      return RejectReason::PriceNotOnTick;
    }
    const std::int64_t notional = cmd.price * cmd.quantity;
    if (notional > inst.max_notional_ticks) {
      return RejectReason::NotionalAboveLimit;
    }
    if (limits.max_notional_ticks > 0 && notional > limits.max_notional_ticks) {
      return RejectReason::NotionalAboveLimit;
    }
  } else {
    // A market order carries no price; a caller that sets one is confused about
    // the semantics, so surface it rather than silently ignoring the field.
    if (cmd.price != 0) {
      return RejectReason::InvalidPrice;
    }
    if (cmd.tif == TimeInForce::Day) {
      return RejectReason::MarketOrderNotAllowedToRest;
    }
  }
  return RejectReason::None;
}

[[nodiscard]] inline RejectReason validate_modify(const Instrument &inst,
                                                  const Command &cmd,
                                                  const RiskLimits &limits) {
  if (cmd.quantity <= 0) {
    return RejectReason::InvalidQuantity;
  }
  if (inst.lot_size > 1 && (cmd.quantity % inst.lot_size) != 0) {
    return RejectReason::InvalidQuantity;
  }
  if (cmd.quantity > inst.max_order_qty) {
    return RejectReason::QuantityAboveLimit;
  }
  if (limits.max_order_qty > 0 && cmd.quantity > limits.max_order_qty) {
    return RejectReason::QuantityAboveLimit;
  }
  if (cmd.price <= 0) {
    return RejectReason::InvalidPrice;
  }
  if (!inst.on_tick(cmd.price)) {
    return RejectReason::PriceNotOnTick;
  }
  const std::int64_t notional = cmd.price * cmd.quantity;
  if (notional > inst.max_notional_ticks) {
    return RejectReason::NotionalAboveLimit;
  }
  if (limits.max_notional_ticks > 0 && notional > limits.max_notional_ticks) {
    return RejectReason::NotionalAboveLimit;
  }
  return RejectReason::None;
}

} // namespace te
