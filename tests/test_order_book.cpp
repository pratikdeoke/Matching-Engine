// Order book structure: side ordering, FIFO time priority, level lifecycle.
#include "TestFixtures.hpp"

using namespace te;
using namespace te::test;

namespace {

// A book side is only reachable through an exchange in normal use, but these
// tests exercise the structure directly to assert ordering without matching
// interfering.
struct BookFixture : public ::testing::Test {
  void SetUp() override {
    inst = table.add("TEST", 1, 100, 1);
    book = std::make_unique<OrderBook>(table.get(inst), pools);
    next_seq = 1;
  }

  Order *add(Side side, Ticks price, Qty qty, ClientId c = kAlice) {
    Order o;
    o.id = OrderId{next_id++};
    o.client = c;
    o.symbol = inst;
    o.price = price;
    o.quantity = qty;
    o.remaining = qty;
    o.side = side;
    o.status = OrderStatus::Accepted;
    o.seq = next_seq++;
    return book->rest(o);
  }

  InstrumentTable table;
  BookPools pools;
  SymbolId inst{};
  std::unique_ptr<OrderBook> book;
  std::uint64_t next_id{1};
  SeqNum next_seq{1};
};

} // namespace

TEST_F(BookFixture, EmptyBookHasNoQuotes) {
  EXPECT_TRUE(book->empty());
  EXPECT_EQ(book->best_bid(), kInvalidTicks);
  EXPECT_EQ(book->best_ask(), kInvalidTicks);
  EXPECT_EQ(book->spread(), kInvalidTicks);
  EXPECT_EQ(book->resting_orders(), 0u);
}

TEST_F(BookFixture, BidsOrderedHighestFirst) {
  add(Side::Buy, 100, 10);
  add(Side::Buy, 105, 10);
  add(Side::Buy, 95, 10);
  add(Side::Buy, 103, 10);

  EXPECT_EQ(book->best_bid(), 105);

  std::vector<DepthEntry> depth;
  book->depth(Side::Buy, 10, depth);
  ASSERT_EQ(depth.size(), 4u);
  EXPECT_EQ(depth[0].price, 105);
  EXPECT_EQ(depth[1].price, 103);
  EXPECT_EQ(depth[2].price, 100);
  EXPECT_EQ(depth[3].price, 95);
}

TEST_F(BookFixture, AsksOrderedLowestFirst) {
  add(Side::Sell, 110, 10);
  add(Side::Sell, 105, 10);
  add(Side::Sell, 120, 10);
  add(Side::Sell, 107, 10);

  EXPECT_EQ(book->best_ask(), 105);

  std::vector<DepthEntry> depth;
  book->depth(Side::Sell, 10, depth);
  ASSERT_EQ(depth.size(), 4u);
  EXPECT_EQ(depth[0].price, 105);
  EXPECT_EQ(depth[1].price, 107);
  EXPECT_EQ(depth[2].price, 110);
  EXPECT_EQ(depth[3].price, 120);
}

TEST_F(BookFixture, SamePriceOrdersAreFifo) {
  Order *first = add(Side::Buy, 100, 10);
  Order *second = add(Side::Buy, 100, 20);
  Order *third = add(Side::Buy, 100, 30);

  const PriceLevel *level = book->bids().best();
  ASSERT_TRUE(level != nullptr);
  EXPECT_EQ(level->count, 3u);
  EXPECT_EQ(level->total_qty, 60);

  // Head is the earliest arrival, tail the latest: this is time priority.
  EXPECT_EQ(level->head, first);
  EXPECT_EQ(level->head->next, second);
  EXPECT_EQ(level->head->next->next, third);
  EXPECT_EQ(level->tail, third);
  EXPECT_TRUE(book->check_invariants());
}

TEST_F(BookFixture, LevelAggregatesQuantity) {
  add(Side::Sell, 200, 5);
  add(Side::Sell, 200, 15);
  add(Side::Sell, 201, 7);

  std::vector<DepthEntry> depth;
  book->depth(Side::Sell, 10, depth);
  ASSERT_EQ(depth.size(), 2u);
  EXPECT_EQ(depth[0].price, 200);
  EXPECT_EQ(depth[0].qty, 20);
  EXPECT_EQ(depth[0].orders, 2u);
  EXPECT_EQ(depth[1].qty, 7);
}

TEST_F(BookFixture, RemovingMiddleOrderKeepsFifoIntact) {
  Order *a = add(Side::Buy, 100, 10);
  Order *b = add(Side::Buy, 100, 20);
  Order *c = add(Side::Buy, 100, 30);
  (void)a;

  book->erase(*b); // unlink from the middle

  const PriceLevel *level = book->bids().best();
  ASSERT_TRUE(level != nullptr);
  EXPECT_EQ(level->count, 2u);
  EXPECT_EQ(level->total_qty, 40);
  EXPECT_EQ(level->head, a);
  EXPECT_EQ(level->tail, c);
  EXPECT_EQ(level->head->next, c);
  EXPECT_EQ(c->prev, a);
  EXPECT_TRUE(book->check_invariants());
}

TEST_F(BookFixture, EmptyLevelIsRemoved) {
  Order *only = add(Side::Buy, 100, 10);
  add(Side::Buy, 99, 10);
  EXPECT_EQ(book->bids().depth(), 2u);

  book->erase(*only);

  // The level must disappear, not linger with zero quantity.
  EXPECT_EQ(book->bids().depth(), 1u);
  EXPECT_EQ(book->best_bid(), 99);
  EXPECT_TRUE(book->check_invariants());
}

TEST_F(BookFixture, LevelPointersSurviveNewLevelInsertion) {
  // Regression guard for the aliasing hazard in a vector-of-levels design:
  // inserting levels must not invalidate resting orders' level back-pointers.
  Order *o = add(Side::Buy, 100, 10);
  const PriceLevel *level_before = o->level;

  for (Ticks p = 101; p < 140; ++p) {
    add(Side::Buy, p, 1); // each becomes the new best, forcing array growth
  }

  EXPECT_EQ(o->level, level_before);
  EXPECT_EQ(o->level->price, 100);
  EXPECT_TRUE(book->check_invariants());
}

TEST_F(BookFixture, SpreadComputedFromBothSides) {
  add(Side::Buy, 100, 10);
  add(Side::Sell, 104, 10);
  EXPECT_EQ(book->spread(), 4);
}

TEST_F(BookFixture, DepthRespectsLimit) {
  for (Ticks p = 90; p <= 100; ++p) {
    add(Side::Buy, p, 1);
  }
  std::vector<DepthEntry> depth;
  book->depth(Side::Buy, 3, depth);
  ASSERT_EQ(depth.size(), 3u);
  EXPECT_EQ(depth[0].price, 100);
  EXPECT_EQ(depth[2].price, 98);
}

TEST_F(BookFixture, ReduceKeepsLevelTotalConsistent) {
  Order *o = add(Side::Sell, 150, 100);
  book->reduce(*o, 40);
  EXPECT_EQ(o->remaining, 60);
  EXPECT_EQ(book->asks().best()->total_qty, 60);
  EXPECT_TRUE(book->check_invariants());
}
