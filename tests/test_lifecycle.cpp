// Order lifecycle: validation rejects, cancellation, modification semantics,
// self-match prevention, and multi-symbol isolation.
#include "TestFixtures.hpp"

using namespace te;
using namespace te::test;

// --- validation ------------------------------------------------------------

TEST_F(ExchangeFixture, RejectsNonPositiveQuantity) {
  buf.clear();
  ex->apply(Command::new_limit(sym, kAlice, OrderId{900}, Side::Buy, 100, 0), buf);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidQuantity);

  buf.clear();
  ex->apply(Command::new_limit(sym, kAlice, OrderId{901}, Side::Buy, 100, -5), buf);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidQuantity);
}

TEST_F(ExchangeFixture, RejectsNonPositivePrice) {
  buf.clear();
  ex->apply(Command::new_limit(sym, kAlice, OrderId{902}, Side::Buy, 0, 10), buf);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidPrice);

  buf.clear();
  ex->apply(Command::new_limit(sym, kAlice, OrderId{903}, Side::Buy, -100, 10), buf);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidPrice);
}

TEST_F(ExchangeFixture, RejectsUnknownSymbol) {
  buf.clear();
  ex->apply(Command::new_limit(SymbolId{9999}, kAlice, OrderId{904}, Side::Buy,
                               100, 10),
            buf);
  EXPECT_EQ(reject_reason(), RejectReason::UnknownSymbol);
}

TEST_F(ExchangeFixture, RejectsDuplicateOrderId) {
  const OrderId id{500};
  buf.clear();
  ex->apply(Command::new_limit(sym, kAlice, id, Side::Buy, 100, 10), buf);
  EXPECT_EQ(rejects(), 0u);

  buf.clear();
  ex->apply(Command::new_limit(sym, kAlice, id, Side::Buy, 101, 10), buf);
  EXPECT_EQ(reject_reason(), RejectReason::DuplicateOrderId);
  // The original must be undisturbed.
  EXPECT_EQ(best_bid(), 100);
}

TEST_F(ExchangeFixture, RejectsPriceOffTick) {
  const SymbolId coarse = ex->add_instrument("TICK5", 5, 100, 1);
  buf.clear();
  ex->apply(Command::new_limit(coarse, kAlice, OrderId{910}, Side::Buy, 102, 10),
            buf);
  EXPECT_EQ(reject_reason(), RejectReason::PriceNotOnTick);

  buf.clear();
  ex->apply(Command::new_limit(coarse, kAlice, OrderId{911}, Side::Buy, 100, 10),
            buf);
  EXPECT_EQ(rejects(), 0u);
}

TEST_F(ExchangeFixture, RejectsQuantityAboveInstrumentLimit) {
  const SymbolId small = ex->add_instrument("SMALL", 1, 100, 1, /*max_qty*/ 100);
  buf.clear();
  ex->apply(Command::new_limit(small, kAlice, OrderId{912}, Side::Buy, 100, 101),
            buf);
  EXPECT_EQ(reject_reason(), RejectReason::QuantityAboveLimit);
}

TEST_F(ExchangeFixture, RejectsLotSizeViolation) {
  const SymbolId lots = ex->add_instrument("LOT10", 1, 100, /*lot*/ 10);
  buf.clear();
  ex->apply(Command::new_limit(lots, kAlice, OrderId{913}, Side::Buy, 100, 15),
            buf);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidQuantity);

  buf.clear();
  ex->apply(Command::new_limit(lots, kAlice, OrderId{914}, Side::Buy, 100, 20),
            buf);
  EXPECT_EQ(rejects(), 0u);
}

// --- cancellation ----------------------------------------------------------

TEST_F(ExchangeFixture, CancelRemovesRestingOrder) {
  const OrderId id = limit(Side::Buy, 100, 10, kAlice);
  EXPECT_TRUE(order_live(id));

  cancel(id, kAlice);

  EXPECT_FALSE(order_live(id));
  EXPECT_TRUE(book().empty());
  ASSERT_EQ(cancels(), 1u);
  const Event *c = buf.first(EventType::OrderCancelled);
  EXPECT_EQ(c->cancel_cause, CancelCause::ClientRequest);
  EXPECT_EQ(c->status, OrderStatus::Cancelled);
}

TEST_F(ExchangeFixture, CancelUnknownOrderRejected) {
  cancel(OrderId{4242}, kAlice);
  EXPECT_EQ(reject_reason(), RejectReason::UnknownOrderId);
}

TEST_F(ExchangeFixture, CancelTwiceRejectsSecondAttempt) {
  const OrderId id = limit(Side::Buy, 100, 10, kAlice);
  cancel(id, kAlice);
  cancel(id, kAlice);
  EXPECT_EQ(reject_reason(), RejectReason::UnknownOrderId);
}

TEST_F(ExchangeFixture, CannotCancelAnotherParticipantsOrder) {
  const OrderId id = limit(Side::Buy, 100, 10, kAlice);
  cancel(id, kBob);
  EXPECT_EQ(reject_reason(), RejectReason::NotOrderOwner);
  // Still live and untouched.
  EXPECT_TRUE(order_live(id));
  EXPECT_EQ(best_bid(), 100);
}

TEST_F(ExchangeFixture, CancelledOrderCannotTrade) {
  const OrderId resting = limit(Side::Sell, 100, 10, kBob);
  cancel(resting, kBob);

  limit(Side::Buy, 100, 10, kAlice);
  EXPECT_EQ(trades(), 0u);
  // The buy rests instead of trading with the cancelled order.
  EXPECT_EQ(best_bid(), 100);
}

TEST_F(ExchangeFixture, FilledOrderIsNoLongerLive) {
  const OrderId sell = limit(Side::Sell, 100, 10, kBob);
  limit(Side::Buy, 100, 10, kAlice);

  EXPECT_FALSE(order_live(sell));
  // And cancelling it now fails, since it is gone.
  cancel(sell, kBob);
  EXPECT_EQ(reject_reason(), RejectReason::UnknownOrderId);
}

TEST_F(ExchangeFixture, CancelMiddleOfQueuePreservesOthers) {
  const OrderId a = limit(Side::Buy, 100, 10, kAlice);
  const OrderId b = limit(Side::Buy, 100, 20, kBob);
  const OrderId c = limit(Side::Buy, 100, 30, kCarol);

  cancel(b, kBob);
  EXPECT_EQ(bid_qty(), 40);

  // a still has priority over c.
  limit(Side::Sell, 100, 10, kCarol);
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting_id, a);
  EXPECT_TRUE(order_live(c));
}

// --- modification ----------------------------------------------------------

TEST_F(ExchangeFixture, QuantityDecreaseRetainsPriority) {
  const OrderId first = limit(Side::Buy, 100, 100, kAlice);
  const OrderId second = limit(Side::Buy, 100, 50, kBob);

  modify(first, 100, 60, kAlice); // same price, smaller size

  const Event *m = buf.first(EventType::OrderModified);
  ASSERT_TRUE(m != nullptr);
  EXPECT_TRUE(m->priority_retained);
  EXPECT_EQ(bid_qty(), 110); // 60 + 50

  // `first` must still be ahead of `second`.
  limit(Side::Sell, 100, 10, kCarol);
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting_id, first);
  (void)second;
}

TEST_F(ExchangeFixture, QuantityIncreaseLosesPriority) {
  const OrderId first = limit(Side::Buy, 100, 50, kAlice);
  const OrderId second = limit(Side::Buy, 100, 50, kBob);

  modify(first, 100, 200, kAlice); // size up => go to the back of the queue

  const Event *m = buf.first(EventType::OrderModified);
  ASSERT_TRUE(m != nullptr);
  EXPECT_FALSE(m->priority_retained);

  // `second` is now first in line.
  limit(Side::Sell, 100, 10, kCarol);
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting_id, second);
  EXPECT_TRUE(order_live(first));
}

TEST_F(ExchangeFixture, PriceChangeLosesPriority) {
  const OrderId first = limit(Side::Buy, 100, 50, kAlice);
  const OrderId second = limit(Side::Buy, 100, 50, kBob);

  // Move away and back: priority must not survive the round trip.
  modify(first, 99, 50, kAlice);
  EXPECT_EQ(best_bid(), 100);
  modify(first, 100, 50, kAlice);

  limit(Side::Sell, 100, 10, kCarol);
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting_id, second);
}

TEST_F(ExchangeFixture, ModifyCanCrossAndTradeImmediately) {
  limit(Side::Sell, 105, 40, kBob);
  const OrderId buy = limit(Side::Buy, 100, 40, kAlice);

  EXPECT_EQ(trades(), 0u);
  modify(buy, 105, 40, kAlice); // repriced through the spread

  EXPECT_EQ(traded_qty(), 40);
  EXPECT_TRUE(book().empty());
}

TEST_F(ExchangeFixture, ModifyPartiallyFilledOrderUsesTotalQuantity) {
  const OrderId sell = limit(Side::Sell, 100, 100, kBob);
  limit(Side::Buy, 100, 30, kAlice); // sell now has 70 remaining of 100

  EXPECT_EQ(view(sell).remaining, 70);

  // New total quantity 50 => residual 20 (50 total minus 30 already filled).
  modify(sell, 100, 50, kBob);
  const OrderView v = view(sell);
  EXPECT_EQ(v.quantity, 50);
  EXPECT_EQ(v.remaining, 20);
  EXPECT_EQ(ask_qty(), 20);
}

TEST_F(ExchangeFixture, ModifyBelowFilledQuantityRejected) {
  const OrderId sell = limit(Side::Sell, 100, 100, kBob);
  limit(Side::Buy, 100, 60, kAlice); // 60 filled

  modify(sell, 100, 60, kBob); // would leave zero outstanding
  EXPECT_EQ(reject_reason(), RejectReason::InvalidModify);
  EXPECT_EQ(view(sell).remaining, 40);
}

TEST_F(ExchangeFixture, ModifyUnknownOrderRejected) {
  modify(OrderId{7777}, 100, 10, kAlice);
  EXPECT_EQ(reject_reason(), RejectReason::UnknownOrderId);
}

TEST_F(ExchangeFixture, CannotModifyAnotherParticipantsOrder) {
  const OrderId id = limit(Side::Buy, 100, 10, kAlice);
  modify(id, 101, 20, kBob);
  EXPECT_EQ(reject_reason(), RejectReason::NotOrderOwner);
  EXPECT_EQ(best_bid(), 100);
  EXPECT_EQ(bid_qty(), 10);
}

TEST_F(ExchangeFixture, ModifyValidatesNewTerms) {
  const OrderId id = limit(Side::Buy, 100, 10, kAlice);
  modify(id, 0, 10, kAlice);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidPrice);
  modify(id, 100, 0, kAlice);
  EXPECT_EQ(reject_reason(), RejectReason::InvalidQuantity);
}

// --- self-match prevention (policy: cancel resting) ------------------------

TEST_F(ExchangeFixture, SelfMatchCancelsRestingOrder) {
  const OrderId resting = limit(Side::Sell, 100, 50, kAlice);
  const OrderId aggressor = limit(Side::Buy, 100, 50, kAlice);

  // No trade between the same participant.
  EXPECT_EQ(trades(), 0u);
  ASSERT_EQ(cancels(), 1u);
  const Event *c = buf.first(EventType::OrderCancelled);
  EXPECT_EQ(c->order_id, resting);
  EXPECT_EQ(c->cancel_cause, CancelCause::SelfMatchPrevention);

  // The aggressor survives and rests.
  EXPECT_TRUE(order_live(aggressor));
  EXPECT_EQ(best_bid(), 100);
  EXPECT_EQ(bid_qty(), 50);
  EXPECT_FALSE(order_live(resting));
}

TEST_F(ExchangeFixture, SelfMatchSkipsOwnOrderThenTradesWithOthers) {
  limit(Side::Sell, 100, 20, kAlice); // Alice's own resting order, first in FIFO
  const OrderId bobs = limit(Side::Sell, 100, 30, kBob);

  limit(Side::Buy, 100, 30, kAlice);

  // Alice's resting order is cancelled; her aggressor trades with Bob's.
  ASSERT_EQ(cancels(), 1u);
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting_id, bobs);
  EXPECT_EQ(t[0].trade_qty, 30);
  EXPECT_TRUE(book().empty());
}

TEST_F(ExchangeFixture, SelfMatchAcrossLevels) {
  limit(Side::Sell, 100, 10, kAlice);
  limit(Side::Sell, 101, 10, kAlice);
  limit(Side::Sell, 102, 10, kBob);

  limit(Side::Buy, 102, 10, kAlice);

  // Both of Alice's resting asks are cancelled, then she trades with Bob at 102.
  EXPECT_EQ(cancels(), 2u);
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].trade_price, 102);
  EXPECT_EQ(t[0].resting_client, kBob);
}

TEST_F(ExchangeFixture, FokAccountsForSelfMatchExclusion) {
  limit(Side::Sell, 100, 40, kAlice); // own liquidity, not tradable by Alice
  limit(Side::Sell, 100, 30, kBob);

  // Only 30 is genuinely fillable for Alice, so a 60-share FOK must reject.
  limit(Side::Buy, 100, 60, kAlice, TimeInForce::Fok);
  EXPECT_EQ(reject_reason(), RejectReason::FillOrKillUnfillable);
  EXPECT_EQ(trades(), 0u);
  // And nothing was cancelled, since FOK is evaluated before any mutation.
  EXPECT_EQ(cancels(), 0u);
  EXPECT_EQ(ask_qty(), 70);
}

TEST_F(ExchangeFixture, MarketOrderSelfMatchPrevented) {
  limit(Side::Sell, 100, 50, kAlice);
  market(Side::Buy, 50, kAlice);

  EXPECT_EQ(trades(), 0u);
  // Resting order cancelled by SMP, market residual cancelled as IOC.
  EXPECT_EQ(cancels(), 2u);
  EXPECT_TRUE(book().empty());
}

// --- multi-symbol isolation ------------------------------------------------

TEST_F(ExchangeFixture, BooksAreIndependentPerSymbol) {
  limit(Side::Buy, 100, 10, kAlice, TimeInForce::Day, sym);
  limit(Side::Sell, 100, 10, kBob, TimeInForce::Day, sym2);

  // Same price, opposite sides, different instruments: no trade.
  EXPECT_EQ(trades(), 0u);
  EXPECT_EQ(book(sym).best_bid(), 100);
  EXPECT_EQ(book(sym).best_ask(), kInvalidTicks);
  EXPECT_EQ(book(sym2).best_ask(), 100);
  EXPECT_EQ(book(sym2).best_bid(), kInvalidTicks);
}

TEST_F(ExchangeFixture, StatsTrackedPerSymbol) {
  limit(Side::Sell, 100, 10, kBob, TimeInForce::Day, sym);
  limit(Side::Buy, 100, 10, kAlice, TimeInForce::Day, sym);

  EXPECT_EQ(ex->engine(sym).stats().trade_count, 1u);
  EXPECT_EQ(ex->engine(sym).stats().volume, 10);
  EXPECT_EQ(ex->engine(sym).stats().last_price, 100);
  EXPECT_EQ(ex->engine(sym2).stats().trade_count, 0u);
}

// --- sequencing ------------------------------------------------------------

TEST_F(ExchangeFixture, EventSequenceIsMonotonic) {
  EventBuffer all;
  ex->apply(Command::new_limit(sym, kBob, OrderId{1}, Side::Sell, 100, 50), all);
  ex->apply(Command::new_limit(sym, kAlice, OrderId{2}, Side::Buy, 100, 20), all);
  ex->apply(Command::cancel(sym, kBob, OrderId{1}), all);

  ASSERT_GT(all.size(), 3u);
  SeqNum prev = 0;
  for (const Event &e : all.events()) {
    EXPECT_GT(e.seq, prev);
    prev = e.seq;
  }
}
