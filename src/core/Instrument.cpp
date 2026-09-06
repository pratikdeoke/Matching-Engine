#include "core/Instrument.hpp"

namespace te {

SymbolId InstrumentTable::add(const std::string &symbol, Ticks tick_size,
                              std::int64_t price_scale, Qty lot_size,
                              Qty max_order_qty,
                              std::int64_t max_notional_ticks) {
  if (auto it = by_symbol_.find(symbol); it != by_symbol_.end()) {
    return it->second;
  }
  const SymbolId id{instruments_.size()};
  Instrument inst;
  inst.id = id;
  inst.symbol = symbol;
  inst.tick_size = tick_size > 0 ? tick_size : 1;
  inst.price_scale = price_scale > 0 ? price_scale : 1;
  inst.lot_size = lot_size > 0 ? lot_size : 1;
  inst.max_order_qty = max_order_qty;
  inst.max_notional_ticks = max_notional_ticks;
  instruments_.push_back(std::move(inst));
  by_symbol_.emplace(symbol, id);
  return id;
}

SymbolId InstrumentTable::find(const std::string &symbol) const {
  if (auto it = by_symbol_.find(symbol); it != by_symbol_.end()) {
    return it->second;
  }
  return SymbolId{};
}

} // namespace te
