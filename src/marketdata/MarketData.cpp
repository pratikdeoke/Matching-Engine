#include "marketdata/MarketData.hpp"

#include <algorithm>

namespace te {

MarketDataPublisher::MarketDataPublisher(const InstrumentTable &instruments)
    : instruments_(&instruments) {
  const std::size_t n = instruments.size();
  tops_.resize(n);
  last_price_.assign(n, kInvalidTicks);
  last_qty_.assign(n, 0);
  trades_.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    tops_[i].symbol = SymbolId{i};
  }
}

void MarketDataPublisher::subscribe(MarketDataSubscriber *sub) {
  if (sub == nullptr) {
    return;
  }
  if (std::find(subscribers_.begin(), subscribers_.end(), sub) ==
      subscribers_.end()) {
    subscribers_.push_back(sub);
  }
}

void MarketDataPublisher::unsubscribe(MarketDataSubscriber *sub) {
  auto it = std::find(subscribers_.begin(), subscribers_.end(), sub);
  if (it != subscribers_.end()) {
    subscribers_.erase(it);
  }
}

void MarketDataPublisher::publish(const Event &event) {
  last_seq_ = event.seq;

  if (event.symbol.value < tops_.size()) {
    // Every event carries a top-of-book stamp, so derived state updates the same
    // way regardless of event type. This is why the engine stamps it.
    TopOfBook &top = tops_[event.symbol.value];
    top.bid = event.best_bid;
    top.bid_qty = event.best_bid_qty;
    top.ask = event.best_ask;
    top.ask_qty = event.best_ask_qty;

    if (event.type == EventType::Trade) {
      last_price_[event.symbol.value] = event.trade_price;
      last_qty_[event.symbol.value] = event.trade_qty;
      auto &hist = trades_[event.symbol.value];
      hist.push_back(event);
      if (hist.size() > trade_capacity_) {
        // Bounded history: drop the oldest half at once rather than erasing the
        // front on every trade, which would be O(n) per trade.
        hist.erase(hist.begin(),
                   hist.begin() +
                       static_cast<std::ptrdiff_t>(trade_capacity_ / 2));
      }
    }
  }

  for (MarketDataSubscriber *sub : subscribers_) {
    sub->on_event(event);
  }
}

void MarketDataPublisher::publish(const std::vector<Event> &events) {
  for (const Event &e : events) {
    publish(e);
  }
}

std::vector<Event> MarketDataPublisher::recent_trades(SymbolId sym,
                                                      std::size_t max_n) const {
  std::vector<Event> out;
  if (sym.value >= trades_.size()) {
    return out;
  }
  const auto &hist = trades_[sym.value];
  const std::size_t n = std::min(max_n, hist.size());
  out.assign(hist.end() - static_cast<std::ptrdiff_t>(n), hist.end());
  return out;
}

} // namespace te
