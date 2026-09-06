#include "exchange/Exchange.hpp"

namespace te {

Exchange::Exchange(ExchangeConfig cfg)
    : cfg_(cfg), pools_{ObjectPool<Order>(cfg.order_pool_chunk),
                        ObjectPool<PriceLevel>(cfg.level_pool_chunk)} {
  if (cfg_.prealloc_orders > 0) {
    pools_.orders.reserve(cfg_.prealloc_orders);
  }
  // Slot 0 mirrors the instrument table's reserved sentinel so that
  // engines_[id.value] is a direct index with no offset arithmetic.
  engines_.emplace_back(nullptr);
}

SymbolId Exchange::add_instrument(const std::string &symbol, Ticks tick_size,
                                  std::int64_t price_scale, Qty lot_size,
                                  Qty max_order_qty,
                                  std::int64_t max_notional_ticks) {
  const SymbolId id = instruments_.add(symbol, tick_size, price_scale, lot_size,
                                       max_order_qty, max_notional_ticks);
  if (id.value < engines_.size() && engines_[id.value] != nullptr) {
    return id; // already registered
  }
  engines_.resize(std::max(engines_.size(), id.value + 1));
  engines_[id.value] = std::make_unique<MatchingEngine>(
      instruments_.get(id), pools_, seqs_, cfg_.limits,
      cfg_.order_index_reserve);
  return id;
}

void Exchange::apply(const Command &cmd, EventBuffer &out) {
  if (!has_symbol(cmd.symbol)) {
    // No engine owns this symbol, so the reject is emitted here. Hand-built
    // rather than routed, since there is no book to stamp a top-of-book from.
    Event e;
    e.type = EventType::OrderRejected;
    e.seq = seqs_.seq();
    e.symbol = cmd.symbol;
    e.order_id = cmd.order_id;
    e.client = cmd.client;
    e.side = cmd.side;
    e.order_type = cmd.order_type;
    e.status = OrderStatus::Rejected;
    e.price = cmd.price;
    e.quantity = cmd.quantity;
    e.reject = RejectReason::UnknownSymbol;
    out.push(e);
    return;
  }
  engines_[cmd.symbol.value]->apply(cmd, out);
}

void Exchange::apply(const Command &cmd) { apply(cmd, events_); }

BookSnapshot Exchange::snapshot(SymbolId s, std::size_t depth) const {
  BookSnapshot snap;
  if (!has_symbol(s)) {
    return snap;
  }
  const MatchingEngine &eng = *engines_[s.value];
  const OrderBook &b = eng.book();
  snap.symbol = s;
  snap.seq = last_seq();
  snap.top.symbol = s;
  snap.top.bid = b.best_bid();
  snap.top.bid_qty = b.best_bid_qty();
  snap.top.ask = b.best_ask();
  snap.top.ask_qty = b.best_ask_qty();
  b.depth(Side::Buy, depth, snap.bids);
  b.depth(Side::Sell, depth, snap.asks);
  snap.last_price = eng.stats().last_price;
  snap.last_qty = eng.stats().last_qty;
  snap.volume = eng.stats().volume;
  snap.trade_count = eng.stats().trade_count;
  return snap;
}

bool Exchange::lookup_order(OrderId id, OrderView &out) const {
  for (const auto &eng : engines_) {
    if (eng != nullptr && eng->lookup(id, out)) {
      return true;
    }
  }
  return false;
}

bool Exchange::check_invariants(std::string *why) const {
  for (const auto &eng : engines_) {
    if (eng == nullptr) {
      continue;
    }
    if (!eng->book().check_invariants(why)) {
      if (why != nullptr) {
        *why = eng->instrument().symbol + ": " + *why;
      }
      return false;
    }
  }
  return true;
}

void Exchange::set_clock(MatchingEngine::Clock c) {
  for (auto &eng : engines_) {
    if (eng != nullptr) {
      eng->set_clock(c);
    }
  }
}

} // namespace te
