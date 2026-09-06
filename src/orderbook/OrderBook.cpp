#include "orderbook/OrderBook.hpp"

#include <algorithm>
#include <string>

namespace te {

// ---------------------------------------------------------------------------
// BookSide
// ---------------------------------------------------------------------------

PriceLevel *BookSide::find_or_create(Ticks price) {
  // Fast path: same price as the current best. Quote-driven flow hits this
  // constantly (repeated adds at the touch), so it is checked before any search.
  if (!levels_.empty() && levels_.back()->price == price) {
    return levels_.back();
  }

  // levels_ is ascending in quality; find the first level that is not worse
  // than `price`. With `worse_than` as the comparator this is a plain
  // lower_bound over the sorted range.
  auto it = std::lower_bound(
      levels_.begin(), levels_.end(), price,
      [this](const PriceLevel *l, Ticks p) { return worse_than(l->price, p); });

  if (it != levels_.end() && (*it)->price == price) {
    return *it;
  }

  PriceLevel *fresh = level_pool_->acquire();
  fresh->price = price;
  fresh->total_qty = 0;
  fresh->count = 0;
  fresh->head = nullptr;
  fresh->tail = nullptr;
  levels_.insert(it, fresh); // moves pointers only; level addresses are stable
  return fresh;
}

void BookSide::drop_level(PriceLevel *level) noexcept {
  // The common case is the best level being consumed, which is an O(1) pop.
  if (!levels_.empty() && levels_.back() == level) {
    levels_.pop_back();
  } else {
    auto it = std::find(levels_.begin(), levels_.end(), level);
    if (it == levels_.end()) {
      return;
    }
    levels_.erase(it);
  }
  level_pool_->release(level);
}

void BookSide::insert(Order &order) {
  PriceLevel *level = find_or_create(order.price);

  // Append at the tail: arrival order == time priority.
  order.prev = level->tail;
  order.next = nullptr;
  order.level = level;
  if (level->tail != nullptr) {
    level->tail->next = &order;
  } else {
    level->head = &order;
  }
  level->tail = &order;

  level->total_qty += order.remaining;
  ++level->count;
}

void BookSide::remove(Order &order) {
  PriceLevel *level = order.level;
  if (level == nullptr) {
    return; // not resting; removal is idempotent
  }

  if (order.prev != nullptr) {
    order.prev->next = order.next;
  } else {
    level->head = order.next;
  }
  if (order.next != nullptr) {
    order.next->prev = order.prev;
  } else {
    level->tail = order.prev;
  }

  level->total_qty -= order.remaining;
  --level->count;
  order.reset_links();

  // Empty level: drop it, so the book never advertises a zero-quantity price
  // and the matcher never has to skip over hollow levels.
  if (level->count == 0) {
    drop_level(level);
  }
}

void BookSide::reduce(Order &order, Qty by) noexcept {
  if (by <= 0) {
    return;
  }
  order.remaining -= by;
  if (order.level != nullptr) {
    order.level->total_qty -= by;
  }
}

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------

OrderBook::OrderBook(const Instrument &instrument, BookPools &pools)
    : instrument_(&instrument), pools_(&pools),
      bids_(Side::Buy, pools.levels), asks_(Side::Sell, pools.levels) {
  // Typical instruments show tens of live price levels; reserving avoids
  // reallocation churn during warm-up without committing much memory.
  bids_.reserve(256);
  asks_.reserve(256);
}

Order *OrderBook::rest(const Order &proto) {
  Order *node = pools_->orders.acquire(proto);
  node->reset_links();
  side(node->side).insert(*node);
  return node;
}

void OrderBook::erase(Order &order) {
  side(order.side).remove(order);
  pools_->orders.release(&order);
}

void OrderBook::depth(Side s, std::size_t max_levels,
                      std::vector<DepthEntry> &out) const {
  out.clear();
  if (max_levels == 0) {
    return;
  }
  out.reserve(std::min(max_levels, side(s).depth()));
  side(s).for_each_level_best_first([&](const PriceLevel &l) {
    if (out.size() >= max_levels) {
      return;
    }
    out.push_back(DepthEntry{l.price, l.total_qty, l.count});
  });
}

namespace {

bool check_side(const BookSide &bs, std::string *why) {
  Ticks prev_price = kInvalidTicks;
  bool first = true;
  bool ok = true;

  bs.for_each_level_best_first([&](const PriceLevel &level) {
    if (!ok) {
      return;
    }
    // Ordering: iterating best-first, each successive level must be strictly
    // worse than the previous one (no duplicates, no inversions).
    if (!first) {
      const bool ordered = bs.side() == Side::Buy ? level.price < prev_price
                                                  : level.price > prev_price;
      if (!ordered) {
        if (why != nullptr) {
          *why = "price levels out of order on " +
                 std::string(to_string(bs.side())) + " side";
        }
        ok = false;
        return;
      }
    }
    first = false;
    prev_price = level.price;

    if (level.head == nullptr || level.tail == nullptr || level.count == 0) {
      if (why != nullptr) {
        *why = "empty price level retained in book";
      }
      ok = false;
      return;
    }

    Qty sum = 0;
    std::uint32_t n = 0;
    SeqNum prev_seq = 0;
    const Order *prev_node = nullptr;
    for (const Order *o = level.head; o != nullptr; o = o->next) {
      if (o->remaining <= 0) {
        if (why != nullptr) {
          *why = "resting order with non-positive remaining quantity";
        }
        ok = false;
        return;
      }
      if (o->price != level.price) {
        if (why != nullptr) {
          *why = "order price does not match its level";
        }
        ok = false;
        return;
      }
      if (o->side != bs.side()) {
        if (why != nullptr) {
          *why = "order resting on the wrong side";
        }
        ok = false;
        return;
      }
      if (o->level != &level) {
        if (why != nullptr) {
          *why = "order level back-pointer is stale";
        }
        ok = false;
        return;
      }
      if (o->prev != prev_node) {
        if (why != nullptr) {
          *why = "FIFO prev links inconsistent";
        }
        ok = false;
        return;
      }
      // Time priority: sequence numbers must increase front-to-back.
      if (prev_node != nullptr && o->seq <= prev_seq) {
        if (why != nullptr) {
          *why = "time priority violated within a price level";
        }
        ok = false;
        return;
      }
      if (!o->active()) {
        if (why != nullptr) {
          *why = "terminal order still resting";
        }
        ok = false;
        return;
      }
      prev_seq = o->seq;
      prev_node = o;
      sum += o->remaining;
      ++n;
    }
    if (level.tail != prev_node) {
      if (why != nullptr) {
        *why = "level tail pointer inconsistent";
      }
      ok = false;
      return;
    }
    if (sum != level.total_qty || n != level.count) {
      if (why != nullptr) {
        *why = "level aggregate qty/count out of sync with its orders";
      }
      ok = false;
    }
  });

  return ok;
}

} // namespace

bool OrderBook::check_invariants(std::string *why) const {
  if (!check_side(bids_, why) || !check_side(asks_, why)) {
    return false;
  }
  // The resting book must never be crossed: if it were, a match was missed.
  const Ticks b = best_bid();
  const Ticks a = best_ask();
  if (b != kInvalidTicks && a != kInvalidTicks && b >= a) {
    if (why != nullptr) {
      *why = "book is crossed: best bid >= best ask";
    }
    return false;
  }
  return true;
}

} // namespace te
