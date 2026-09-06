#include "tools/WorkloadGenerator.hpp"

#include <algorithm>

namespace te {

WorkloadGenerator::WorkloadGenerator(WorkloadConfig cfg,
                                     std::vector<SymbolId> symbols)
    : cfg_(cfg), symbols_(std::move(symbols)), rng_(cfg.seed) {
  if (symbols_.empty()) {
    symbols_.push_back(SymbolId{1});
  }
  refs_.assign(symbols_.size(), cfg_.ref_price);
  live_.reserve(cfg_.num_commands / 4 + 16);
}

void WorkloadGenerator::reset() {
  rng_.reseed(cfg_.seed);
  next_order_id_ = 1;
  live_.clear();
  refs_.assign(symbols_.size(), cfg_.ref_price);
}

std::vector<Command> WorkloadGenerator::generate() {
  reset();
  std::vector<Command> out;
  out.reserve(cfg_.num_commands);
  for (std::size_t i = 0; i < cfg_.num_commands; ++i) {
    out.push_back(next_command());
  }
  return out;
}

Command WorkloadGenerator::next_command() {
  const std::uint32_t roll = static_cast<std::uint32_t>(rng_.below(100));

  // Cancel or modify an order we believe is live. Falls through to a new order
  // when nothing is tracked yet (start of the stream).
  if (!live_.empty() && roll < cfg_.pct_cancel) {
    const std::size_t idx = static_cast<std::size_t>(rng_.below(live_.size()));
    const LiveOrder tgt = live_[idx];
    // Remove from the tracking set: cancelling twice would just generate a
    // predictable reject and tell us nothing.
    live_[idx] = live_.back();
    live_.pop_back();
    return Command::cancel(tgt.symbol, tgt.client, tgt.id);
  }

  if (!live_.empty() && roll < cfg_.pct_cancel + cfg_.pct_modify) {
    const std::size_t idx = static_cast<std::size_t>(rng_.below(live_.size()));
    const LiveOrder tgt = live_[idx];
    const std::size_t sidx = static_cast<std::size_t>(
        std::find(symbols_.begin(), symbols_.end(), tgt.symbol) -
        symbols_.begin());
    const Ticks ref = refs_[std::min(sidx, refs_.size() - 1)];
    // Reprice near the reference and resize. Both a price move and a size change
    // are exercised, which is what makes the priority rules get hit.
    Ticks price = ref + static_cast<Ticks>(rng_.range(-cfg_.price_band, cfg_.price_band));
    price = std::max<Ticks>(cfg_.tick_size, (price / cfg_.tick_size) * cfg_.tick_size);
    Qty qty = static_cast<Qty>(rng_.range(cfg_.min_qty, cfg_.max_qty));
    qty = std::max<Qty>(cfg_.lot_size, (qty / cfg_.lot_size) * cfg_.lot_size);
    return Command::modify(tgt.symbol, tgt.client, tgt.id, price, qty);
  }

  const std::size_t sidx = static_cast<std::size_t>(rng_.below(symbols_.size()));
  return make_new_order(symbols_[sidx]);
}

Command WorkloadGenerator::make_new_order(SymbolId sym) {
  const std::size_t sidx = static_cast<std::size_t>(
      std::find(symbols_.begin(), symbols_.end(), sym) - symbols_.begin());
  Ticks &ref = refs_[std::min(sidx, refs_.size() - 1)];

  // Reference random-walks by at most a tick per order, so the book drifts and
  // stale levels get swept — this is what produces multi-level fills.
  ref += static_cast<Ticks>(rng_.range(-1, 1)) * cfg_.tick_size;
  ref = std::max<Ticks>(cfg_.price_band + cfg_.tick_size, ref);

  const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
  const ClientId client{1 + rng_.below(cfg_.num_clients)};
  const OrderId oid{next_order_id_++};

  Qty qty = static_cast<Qty>(rng_.range(cfg_.min_qty, cfg_.max_qty));
  qty = std::max<Qty>(cfg_.lot_size, (qty / cfg_.lot_size) * cfg_.lot_size);

  if (rng_.chance(cfg_.pct_market)) {
    // Market orders never rest, so they are not tracked as live.
    return Command::new_market(sym, client, oid, side, qty);
  }

  Ticks price;
  if (rng_.chance(cfg_.pct_aggressive)) {
    // Price through the reference to cross the spread.
    const Ticks through =
        static_cast<Ticks>(rng_.range(0, cfg_.price_band / 2)) * cfg_.tick_size;
    price = side == Side::Buy ? ref + through : ref - through;
  } else {
    // Passive: rest away from the reference on the correct side.
    const Ticks away =
        static_cast<Ticks>(rng_.range(1, cfg_.price_band)) * cfg_.tick_size;
    price = side == Side::Buy ? ref - away : ref + away;
  }
  price = std::max<Ticks>(cfg_.tick_size, (price / cfg_.tick_size) * cfg_.tick_size);

  const bool ioc = rng_.chance(cfg_.pct_ioc);
  const TimeInForce tif = ioc ? TimeInForce::Ioc : TimeInForce::Day;

  if (!ioc) {
    // Only Day orders can rest, so only they are candidates for later
    // cancel/modify.
    live_.push_back(LiveOrder{oid, client, sym});
  }
  return Command::new_limit(sym, client, oid, side, price, qty, tif);
}

} // namespace te
