#include "matching/MatchingEngine.hpp"

#include <algorithm>

namespace te {

MatchingEngine::MatchingEngine(const Instrument &instrument, BookPools &pools,
                               SequenceSource &seqs, RiskLimits limits,
                               std::size_t index_reserve)
    : book_(instrument, pools), seqs_(&seqs), limits_(limits) {
  index_.reserve(index_reserve);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void MatchingEngine::apply(const Command &cmd, EventBuffer &out) {
  switch (cmd.type) {
  case CommandType::NewOrder:
    handle_new(cmd, out);
    break;
  case CommandType::CancelOrder:
    handle_cancel(cmd, out);
    break;
  case CommandType::ModifyOrder:
    handle_modify(cmd, out);
    break;
  }
}

// ---------------------------------------------------------------------------
// New order
// ---------------------------------------------------------------------------

void MatchingEngine::handle_new(const Command &cmd, EventBuffer &out) {
  const Instrument &inst = book_.instrument();

  if (const RejectReason r = validate_new_order(inst, cmd, limits_);
      r != RejectReason::None) {
    emit_rejected(cmd, r, out);
    return;
  }
  if (index_.contains(cmd.order_id)) {
    emit_rejected(cmd, RejectReason::DuplicateOrderId, out);
    return;
  }

  // A market order into an empty opposite side has nothing to do. Reject rather
  // than accept-then-cancel: the client learns the reason in one message.
  const bool market = cmd.order_type == OrderType::Market;
  if (market && book_.side(opposite(cmd.side)).empty()) {
    emit_rejected(cmd, RejectReason::NoLiquidity, out);
    return;
  }

  // The aggressor's effective limit. A market order is modelled as a limit at
  // the most permissive price, which lets one matching loop serve both types
  // without a per-iteration branch on order type.
  const Ticks limit =
      market ? (cmd.side == Side::Buy ? kMaxTicks : 1) : cmd.price;

  // FOK is decided before any mutation, so an unfillable FOK leaves no trace
  // beyond its reject.
  if (cmd.tif == TimeInForce::Fok) {
    if (fillable(opposite(cmd.side), limit, cmd.client, cmd.order_type) <
        cmd.quantity) {
      emit_rejected(cmd, RejectReason::FillOrKillUnfillable, out);
      return;
    }
  }

  // Build the aggressor on the stack. It is copied into the pool only if it
  // survives to rest, so fully-filled aggressive orders never touch the pool.
  Order aggressor;
  aggressor.id = cmd.order_id;
  aggressor.client = cmd.client;
  aggressor.symbol = inst.id;
  aggressor.price = limit;
  aggressor.quantity = cmd.quantity;
  aggressor.remaining = cmd.quantity;
  aggressor.side = cmd.side;
  aggressor.type = cmd.order_type;
  aggressor.tif = cmd.tif;
  aggressor.status = OrderStatus::Accepted;
  aggressor.seq = seqs_->seq();
  aggressor.ts_accepted = now();

  ++stats_.orders_accepted;
  emit_accepted(aggressor, out);

  match(aggressor, out);

  if (aggressor.remaining > 0) {
    const bool rests = !market && cmd.tif == TimeInForce::Day;
    if (rests) {
      // Restore the true limit price before resting (identical for limit orders,
      // but keeps the invariant explicit).
      aggressor.price = cmd.price;
      rest_order(aggressor, out);
    } else {
      // IOC residual (and market residual) is cancelled, not rested.
      aggressor.status = OrderStatus::Cancelled;
      emit_cancelled(aggressor, CancelCause::IocResidual, out);
    }
  }

  emit_book_update(out);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

void MatchingEngine::match(Order &aggressor, EventBuffer &out) {
  BookSide &contra = book_.side(opposite(aggressor.side));

  while (aggressor.remaining > 0) {
    PriceLevel *level = contra.best();
    if (level == nullptr) {
      break; // no liquidity left on the contra side
    }
    // Price eligibility: a buy aggressor trades at asks <= its limit, a sell at
    // bids >= its limit. Integer comparison, no epsilon.
    const bool eligible = aggressor.side == Side::Buy
                              ? level->price <= aggressor.price
                              : level->price >= aggressor.price;
    if (!eligible) {
      break;
    }

    const Ticks trade_price = level->price; // resting order sets the price

    // Consume the level FIFO front-first: this is where time priority is
    // enforced.
    //
    // `level` must be re-read from contra.best() on each iteration rather than
    // cached: removing the last order at this price returns the level to the
    // pool, so the pointer would dangle. Re-reading is one load from a hot
    // vector and keeps the lifetime rule local and obvious.
    while (aggressor.remaining > 0) {
      PriceLevel *cur = contra.best();
      if (cur == nullptr || cur->price != trade_price || cur->head == nullptr) {
        break; // this price is exhausted (or gone)
      }
      Order &resting = *cur->head;

      // Self-match prevention: cancel the resting order and keep sweeping.
      if (resting.client == aggressor.client) {
        resting.status = OrderStatus::Cancelled;
        emit_cancelled(resting, CancelCause::SelfMatchPrevention, out);
        ++stats_.orders_cancelled;
        remove_resting(resting); // may free `cur`
        continue;
      }

      const Qty qty = std::min(aggressor.remaining, resting.remaining);
      const bool resting_done = resting.remaining == qty;

      // Capture what the trade event needs before the resting node can be
      // recycled, so the event can be emitted *after* the book is fully updated
      // and therefore carries a correct post-trade top-of-book snapshot.
      const OrderId resting_id = resting.id;
      const ClientId resting_client = resting.client;

      aggressor.remaining -= qty;
      contra.reduce(resting, qty);
      resting.status =
          resting_done ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
      if (resting_done) {
        remove_resting(resting); // may free `cur`
      }

      emit_trade(aggressor, resting_id, resting_client, trade_price, qty, out);
    }
  }

  if (aggressor.remaining == 0) {
    aggressor.status = OrderStatus::Filled;
  } else if (aggressor.remaining < aggressor.quantity) {
    aggressor.status = OrderStatus::PartiallyFilled;
  }
}

Qty MatchingEngine::fillable(Side side, Ticks limit, ClientId client,
                             OrderType /*type*/) const {
  Qty total = 0;
  const BookSide &contra = book_.side(side);
  contra.for_each_level_best_first([&](const PriceLevel &level) {
    const bool eligible =
        side == Side::Sell ? level.price <= limit : level.price >= limit;
    if (!eligible) {
      return;
    }
    // Walk the level's orders so that the client's own resting quantity (which
    // SMP would cancel, not trade) is excluded from the fillable total.
    for (const Order *o = level.head; o != nullptr; o = o->next) {
      if (o->client != client) {
        total += o->remaining;
      }
    }
  });
  return total;
}

// ---------------------------------------------------------------------------
// Resting / index maintenance
// ---------------------------------------------------------------------------

void MatchingEngine::rest_order(const Order &proto, EventBuffer &out) {
  Order *node = book_.rest(proto);
  index_.insert_or_assign(node->id, node);
  (void)out; // acceptance was already reported; resting adds no separate event
}

void MatchingEngine::remove_resting(Order &order) {
  index_.erase(order.id);
  book_.erase(order); // unlinks and returns the node to the pool
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

void MatchingEngine::handle_cancel(const Command &cmd, EventBuffer &out) {
  Order **slot = index_.find(cmd.order_id);
  if (slot == nullptr) {
    emit_rejected(cmd, RejectReason::UnknownOrderId, out);
    return;
  }
  Order &order = **slot;

  // Ownership check: a participant may only cancel their own order.
  if (order.client != cmd.client) {
    emit_rejected(cmd, RejectReason::NotOrderOwner, out);
    return;
  }
  if (!order.active()) {
    emit_rejected(cmd, RejectReason::OrderNotActive, out);
    return;
  }

  order.status = OrderStatus::Cancelled;
  ++stats_.orders_cancelled;
  emit_cancelled(order, CancelCause::ClientRequest, out);
  remove_resting(order);
  emit_book_update(out);
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

void MatchingEngine::handle_modify(const Command &cmd, EventBuffer &out) {
  Order **slot = index_.find(cmd.order_id);
  if (slot == nullptr) {
    emit_rejected(cmd, RejectReason::UnknownOrderId, out);
    return;
  }
  Order *order = *slot;
  if (order->client != cmd.client) {
    emit_rejected(cmd, RejectReason::NotOrderOwner, out);
    return;
  }
  if (!order->active()) {
    emit_rejected(cmd, RejectReason::OrderNotActive, out);
    return;
  }
  if (const RejectReason r = validate_modify(book_.instrument(), cmd, limits_);
      r != RejectReason::None) {
    emit_rejected(cmd, r, out);
    return;
  }

  const Ticks old_price = order->price;
  const Qty old_qty = order->quantity;
  const Qty already_filled = order->filled();

  // The new quantity is the *total* order quantity, so the residual it implies
  // is new_qty - already_filled. A modify that would leave nothing outstanding
  // is an error: use cancel.
  const Qty new_remaining = cmd.quantity - already_filled;
  if (new_remaining <= 0) {
    emit_rejected(cmd, RejectReason::InvalidModify, out);
    return;
  }

  const bool price_changed = cmd.price != old_price;
  const bool qty_increased = new_remaining > order->remaining;
  const bool loses_priority = price_changed || qty_increased;

  if (!loses_priority) {
    // Pure quantity reduction at the same price: adjust in place and keep the
    // order's position in the FIFO.
    const Qty delta = order->remaining - new_remaining;
    book_.side(order->side).reduce(*order, delta);
    order->quantity = cmd.quantity;
    ++stats_.orders_modified;

    Event e;
    e.type = EventType::OrderModified;
    e.seq = seqs_->seq();
    e.ts = now();
    e.symbol = order->symbol;
    e.order_id = order->id;
    e.client = order->client;
    e.side = order->side;
    e.order_type = order->type;
    e.status = order->status;
    e.price = order->price;
    e.quantity = order->quantity;
    e.remaining = order->remaining;
    e.old_price = old_price;
    e.old_quantity = old_qty;
    e.priority_retained = true;
    stamp_top_of_book(e);
    out.push(e);

    emit_book_update(out);
    return;
  }

  // Priority-losing modify == cancel/replace. Copy the terms out, drop the node,
  // then re-enter as a fresh aggressor with a new sequence number. Re-entering
  // through the matcher is required for correctness: the new price may cross.
  Order replacement = *order;
  replacement.price = cmd.price;
  replacement.quantity = cmd.quantity;
  replacement.remaining = new_remaining;
  replacement.seq = seqs_->seq();
  replacement.ts_accepted = now();
  replacement.reset_links();
  replacement.status = already_filled > 0 ? OrderStatus::PartiallyFilled
                                          : OrderStatus::Accepted;

  remove_resting(*order);
  order = nullptr; // node is back in the pool; must not be touched again
  ++stats_.orders_modified;

  Event e;
  e.type = EventType::OrderModified;
  e.seq = replacement.seq;
  e.ts = replacement.ts_accepted;
  e.symbol = replacement.symbol;
  e.order_id = replacement.id;
  e.client = replacement.client;
  e.side = replacement.side;
  e.order_type = replacement.type;
  e.status = replacement.status;
  e.price = replacement.price;
  e.quantity = replacement.quantity;
  e.remaining = replacement.remaining;
  e.old_price = old_price;
  e.old_quantity = old_qty;
  e.priority_retained = false;
  stamp_top_of_book(e);
  out.push(e);

  match(replacement, out);

  if (replacement.remaining > 0) {
    rest_order(replacement, out);
  }
  emit_book_update(out);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool MatchingEngine::lookup(OrderId id, OrderView &out) const {
  const Order *const *slot = index_.find(id);
  if (slot == nullptr) {
    return false;
  }
  out = OrderView::of(**slot);
  return true;
}

// ---------------------------------------------------------------------------
// Event emission
// ---------------------------------------------------------------------------

void MatchingEngine::stamp_top_of_book(Event &e) const {
  e.best_bid = book_.best_bid();
  e.best_bid_qty = book_.best_bid_qty();
  e.best_ask = book_.best_ask();
  e.best_ask_qty = book_.best_ask_qty();
}

void MatchingEngine::emit_accepted(const Order &o, EventBuffer &out) {
  Event e;
  e.type = EventType::OrderAccepted;
  e.seq = o.seq; // acceptance shares the order's sequence number
  e.ts = o.ts_accepted;
  e.symbol = o.symbol;
  e.order_id = o.id;
  e.client = o.client;
  e.side = o.side;
  e.order_type = o.type;
  e.status = OrderStatus::Accepted;
  e.price = o.type == OrderType::Market ? 0 : o.price;
  e.quantity = o.quantity;
  e.remaining = o.remaining;
  stamp_top_of_book(e);
  out.push(e);
}

void MatchingEngine::emit_rejected(const Command &cmd, RejectReason reason,
                                   EventBuffer &out) {
  ++stats_.orders_rejected;
  Event e;
  e.type = EventType::OrderRejected;
  e.seq = seqs_->seq();
  e.ts = now();
  e.symbol = cmd.symbol;
  e.order_id = cmd.order_id;
  e.client = cmd.client;
  e.side = cmd.side;
  e.order_type = cmd.order_type;
  e.status = OrderStatus::Rejected;
  e.price = cmd.price;
  e.quantity = cmd.quantity;
  e.remaining = 0;
  e.reject = reason;
  stamp_top_of_book(e);
  out.push(e);
}

void MatchingEngine::emit_cancelled(const Order &o, CancelCause cause,
                                    EventBuffer &out) {
  Event e;
  e.type = EventType::OrderCancelled;
  e.seq = seqs_->seq();
  e.ts = now();
  e.symbol = o.symbol;
  e.order_id = o.id;
  e.client = o.client;
  e.side = o.side;
  e.order_type = o.type;
  e.status = OrderStatus::Cancelled;
  e.price = o.type == OrderType::Market ? 0 : o.price;
  e.quantity = o.quantity;
  e.remaining = o.remaining;
  e.cancel_cause = cause;
  stamp_top_of_book(e);
  out.push(e);
}

void MatchingEngine::emit_trade(const Order &aggressor, OrderId resting_id,
                                ClientId resting_client, Ticks price, Qty qty,
                                EventBuffer &out) {
  stats_.last_price = price;
  stats_.last_qty = qty;
  ++stats_.trade_count;
  stats_.volume += qty;
  stats_.turnover_ticks += price * qty;

  Event e;
  e.type = EventType::Trade;
  e.seq = seqs_->seq();
  e.ts = now();
  e.symbol = aggressor.symbol;
  e.trade_id = seqs_->trade();
  e.trade_price = price;
  e.trade_qty = qty;
  e.aggressor_id = aggressor.id;
  e.resting_id = resting_id;
  e.aggressor_client = aggressor.client;
  e.resting_client = resting_client;
  e.aggressor_side = aggressor_of(aggressor.side);
  // Mirror into the order-scoped fields so a consumer that only reads
  // price/quantity gets sensible values for a trade too.
  e.order_id = aggressor.id;
  e.client = aggressor.client;
  e.side = aggressor.side;
  e.order_type = aggressor.type;
  e.price = price;
  e.quantity = qty;
  e.remaining = aggressor.remaining;
  stamp_top_of_book(e);
  out.push(e);
}

void MatchingEngine::emit_book_update(EventBuffer &out) {
  Event e;
  e.type = EventType::BookUpdate;
  e.seq = seqs_->seq();
  e.ts = now();
  e.symbol = book_.symbol();
  e.price = stats_.last_price == kInvalidTicks ? 0 : stats_.last_price;
  e.quantity = stats_.last_qty;
  stamp_top_of_book(e);
  out.push(e);
}

} // namespace te
