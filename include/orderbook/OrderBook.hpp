// Single-instrument limit order book with strict price-time priority.
//
// ---------------------------------------------------------------------------
// Data structure decision
// ---------------------------------------------------------------------------
// Each side holds a *sorted std::vector<PriceLevel>* ordered so that the best
// price is at the back(), not a std::map<Ticks, Level>.
//
// Rationale:
//   * The matcher only ever touches the best level, repeatedly. back() is a
//     single dereference into a contiguous array; std::map::begin() chases a
//     red-black tree node in unrelated memory.
//   * Consuming a level is pop_back() — no node deallocation, no rebalancing.
//   * Level insertion is a lower_bound + vector insert. That is O(n) memmove in
//     theory, but real order flow concentrates near the touch: a new level is
//     almost always inserted at or near the end, and the moved payload is small
//     (a level is price + qty + count + two pointers). Contiguity wins over the
//     asymptotics at realistic depths.
//   * Sorting so that best == back() means the common case (new best price, or
//     depleting the best) is an O(1) push_back/pop_back with no shifting.
//
// Bids are sorted ascending by price  => back() is the highest bid.
// Asks are sorted descending by price => back() is the lowest ask.
//
// Important consequence: the vector holds *pointers* to pool-allocated
// PriceLevel objects, not the objects themselves. Resting orders keep an
// intrusive back-pointer to their level so that cancel is O(1), and inserting a
// new price level must not invalidate it. Storing levels by value would move
// them on every insert and leave every resting order's back-pointer dangling.
// The pointer array stays contiguous, so scanning depth is still linear in
// memory; only the level payload is indirect, and the matcher touches one level
// at a time.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Instrument.hpp"
#include "core/Order.hpp"
#include "core/Types.hpp"
#include "util/ObjectPool.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace te {

// A FIFO queue of orders resting at one price.
struct PriceLevel {
  Ticks price{0};
  Qty total_qty{0};      // sum of remaining quantity, maintained incrementally
  std::uint32_t count{0}; // number of resting orders
  Order *head{nullptr};   // front of FIFO: highest time priority
  Order *tail{nullptr};   // back of FIFO: most recently arrived

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

// One side of the book. Kept as its own type so bid/ask asymmetry lives in one
// place (the comparator) instead of being branched on throughout the matcher.
class BookSide {
public:
  BookSide(Side side, ObjectPool<PriceLevel> &level_pool)
      : side_(side), level_pool_(&level_pool) {}

  ~BookSide() { clear(); }

  BookSide(const BookSide &) = delete;
  BookSide &operator=(const BookSide &) = delete;

  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
  [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }

  // Best level (highest bid / lowest ask), or nullptr when the side is empty.
  [[nodiscard]] PriceLevel *best() noexcept {
    return levels_.empty() ? nullptr : levels_.back();
  }
  [[nodiscard]] const PriceLevel *best() const noexcept {
    return levels_.empty() ? nullptr : levels_.back();
  }

  [[nodiscard]] Ticks best_price() const noexcept {
    return levels_.empty() ? kInvalidTicks : levels_.back()->price;
  }
  [[nodiscard]] Qty best_qty() const noexcept {
    return levels_.empty() ? 0 : levels_.back()->total_qty;
  }

  // Appends the order at the back of its price level's FIFO, creating the level
  // if needed. This is the only way an order gains time priority, so priority is
  // by construction arrival-ordered within a price.
  void insert(Order &order);

  // Unlinks a resting order. O(1): we have the node, its neighbours and its
  // level. Removes the level when it empties.
  void remove(Order &order);

  // Reduces a resting order's remaining quantity by `by`, keeping level totals
  // consistent. Used by the matcher on partial fills of the resting side.
  void reduce(Order &order, Qty by) noexcept;

  // Levels ordered best-first, for depth snapshots.
  template <typename Fn> void for_each_level_best_first(Fn &&fn) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
      fn(**it);
    }
  }

  // True when `price` is at or through the best price on this side, i.e. an
  // incoming order at `price` from the opposite side would trade here.
  [[nodiscard]] bool crosses(Ticks price) const noexcept {
    if (levels_.empty()) {
      return false;
    }
    const Ticks best_p = levels_.back()->price;
    return side_ == Side::Buy ? price <= best_p : price >= best_p;
  }

  void reserve(std::size_t n) { levels_.reserve(n); }

  void clear() noexcept {
    for (PriceLevel *l : levels_) {
      level_pool_->release(l);
    }
    levels_.clear();
  }

  [[nodiscard]] std::size_t order_count() const noexcept {
    std::size_t n = 0;
    for (const PriceLevel *l : levels_) {
      n += l->count;
    }
    return n;
  }

private:
  // Strictly-worse-than ordering, so the vector is ascending in "quality" and
  // the best level lands at back().
  [[nodiscard]] bool worse_than(Ticks a, Ticks b) const noexcept {
    return side_ == Side::Buy ? a < b : a > b;
  }

  // Finds the level for `price`, creating it at the correct sorted position if
  // absent.
  PriceLevel *find_or_create(Ticks price);

  // Erases an empty level from the sorted array and returns it to the pool.
  void drop_level(PriceLevel *level) noexcept;

  Side side_;
  ObjectPool<PriceLevel> *level_pool_;
  std::vector<PriceLevel *> levels_; // ascending in quality; best == back()
};

// Aggregated depth snapshot entry, for market data and the REST API.
struct DepthEntry {
  Ticks price{0};
  Qty qty{0};
  std::uint32_t orders{0};
};

// Pools shared by every book on one matching shard. Bundled into a struct so
// adding a pool later does not change every constructor signature. A shard is
// single-threaded, so these need no synchronisation; sharing across the shard's
// instruments keeps total slack memory far below one pool per symbol.
struct BookPools {
  ObjectPool<Order> orders{8192};
  ObjectPool<PriceLevel> levels{1024};
};

class OrderBook {
public:
  // The book borrows the pools rather than owning them.
  OrderBook(const Instrument &instrument, BookPools &pools);

  [[nodiscard]] const Instrument &instrument() const noexcept { return *instrument_; }
  [[nodiscard]] SymbolId symbol() const noexcept { return instrument_->id; }

  [[nodiscard]] BookSide &bids() noexcept { return bids_; }
  [[nodiscard]] BookSide &asks() noexcept { return asks_; }
  [[nodiscard]] const BookSide &bids() const noexcept { return bids_; }
  [[nodiscard]] const BookSide &asks() const noexcept { return asks_; }

  [[nodiscard]] BookSide &side(Side s) noexcept {
    return s == Side::Buy ? bids_ : asks_;
  }
  [[nodiscard]] const BookSide &side(Side s) const noexcept {
    return s == Side::Buy ? bids_ : asks_;
  }

  [[nodiscard]] Ticks best_bid() const noexcept { return bids_.best_price(); }
  [[nodiscard]] Ticks best_ask() const noexcept { return asks_.best_price(); }
  [[nodiscard]] Qty best_bid_qty() const noexcept { return bids_.best_qty(); }
  [[nodiscard]] Qty best_ask_qty() const noexcept { return asks_.best_qty(); }

  // Spread in ticks, or kInvalidTicks when either side is empty.
  [[nodiscard]] Ticks spread() const noexcept {
    const Ticks b = best_bid();
    const Ticks a = best_ask();
    return (b == kInvalidTicks || a == kInvalidTicks) ? kInvalidTicks : a - b;
  }

  // --- resting order storage -------------------------------------------
  // Creates a pooled Order and rests it. Returns the stable node address.
  Order *rest(const Order &proto);

  // Removes and destroys a resting order.
  void erase(Order &order);

  void reduce(Order &order, Qty by) noexcept { side(order.side).reduce(order, by); }

  [[nodiscard]] std::size_t resting_orders() const noexcept {
    return bids_.order_count() + asks_.order_count();
  }
  [[nodiscard]] bool empty() const noexcept {
    return bids_.empty() && asks_.empty();
  }

  // --- market data ------------------------------------------------------
  void depth(Side s, std::size_t max_levels, std::vector<DepthEntry> &out) const;

  // --- invariant check, used by tests and the fuzz/property harness ------
  // Verifies: level ordering, per-level totals vs the FIFO contents, FIFO
  // sequence monotonicity (time priority), no non-positive remaining quantity,
  // and that the book is not crossed.
  [[nodiscard]] bool check_invariants(std::string *why = nullptr) const;

private:
  const Instrument *instrument_;
  BookPools *pools_;
  BookSide bids_;
  BookSide asks_;
};

} // namespace te
