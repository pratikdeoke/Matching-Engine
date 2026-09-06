// Matching semantics: price-time priority, fills, sweeps, market orders, TIF.
#include "TestFixtures.hpp"

using namespace te;
using namespace te::test;

TEST_F(ExchangeFixture, NoMatchWhenPricesDoNotCross) {
  limit(Side::Buy, 100, 10, kAlice);
  limit(Side::Sell, 101, 10, kBob);

  EXPECT_EQ(trades(), 0u);
  EXPECT_EQ(best_bid(), 100);
  EXPECT_EQ(best_ask(), 101);
  EXPECT_EQ(book().spread(), 1);
}

TEST_F(ExchangeFixture, ExactMatchClearsBothOrders) {
  limit(Side::Sell, 100, 50, kBob);
  limit(Side::Buy, 100, 50, kAlice);

  ASSERT_EQ(trades(), 1u);
  const auto t = trade_events();
  EXPECT_EQ(t[0].trade_price, 100);
  EXPECT_EQ(t[0].trade_qty, 50);
  // Book fully cleared: no residual on either side.
  EXPECT_TRUE(book().empty());
}

TEST_F(ExchangeFixture, TradePrintsAtRestingPrice) {
  // Resting sell at 100; buyer willing to pay 105. The resting order set the
  // price, so the trade prints at 100 and the buyer gets price improvement.
  limit(Side::Sell, 100, 10, kBob);
  limit(Side::Buy, 105, 10, kAlice);

  ASSERT_EQ(trades(), 1u);
  EXPECT_EQ(trade_events()[0].trade_price, 100);
}

TEST_F(ExchangeFixture, AggressorPartiallyFilledRests) {
  limit(Side::Sell, 100, 30, kBob);
  const OrderId buy = limit(Side::Buy, 100, 100, kAlice);

  EXPECT_EQ(traded_qty(), 30);
  // Residual 70 rests as the new best bid.
  EXPECT_EQ(best_bid(), 100);
  EXPECT_EQ(bid_qty(), 70);
  EXPECT_TRUE(book().asks().empty());

  const OrderView v = view(buy);
  EXPECT_EQ(v.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(v.remaining, 70);
  EXPECT_EQ(v.quantity, 100);
}

TEST_F(ExchangeFixture, RestingOrderPartiallyFilledStaysWithReducedQty) {
  const OrderId sell = limit(Side::Sell, 100, 100, kBob);
  limit(Side::Buy, 100, 40, kAlice);

  EXPECT_EQ(traded_qty(), 40);
  EXPECT_EQ(ask_qty(), 60);
  const OrderView v = view(sell);
  EXPECT_EQ(v.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(v.remaining, 60);
}

TEST_F(ExchangeFixture, MultiLevelSweepFollowsPriceOrder) {
  // Book: 100 @ 101, 50 @ 102, 100 @ 105  (the scenario from the spec)
  limit(Side::Sell, 101, 100, kBob);
  limit(Side::Sell, 102, 50, kBob);
  limit(Side::Sell, 105, 100, kBob);

  limit(Side::Buy, 105, 100, kAlice);

  // 100 taken entirely from the best level at 101; nothing else needed.
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].trade_price, 101);
  EXPECT_EQ(t[0].trade_qty, 100);
  EXPECT_EQ(best_ask(), 102);
}

TEST_F(ExchangeFixture, SweepConsumesLevelsCheapestFirst) {
  limit(Side::Sell, 101, 100, kBob);
  limit(Side::Sell, 102, 50, kBob);
  limit(Side::Sell, 105, 100, kBob);

  // 200 shares at up to 105 must take 100@101, 50@102, then 50@105.
  limit(Side::Buy, 105, 200, kAlice);

  const auto t = trade_events();
  ASSERT_EQ(t.size(), 3u);
  EXPECT_EQ(t[0].trade_price, 101);
  EXPECT_EQ(t[0].trade_qty, 100);
  EXPECT_EQ(t[1].trade_price, 102);
  EXPECT_EQ(t[1].trade_qty, 50);
  EXPECT_EQ(t[2].trade_price, 105);
  EXPECT_EQ(t[2].trade_qty, 50);

  EXPECT_EQ(traded_qty(), 200);
  EXPECT_EQ(best_ask(), 105);
  EXPECT_EQ(ask_qty(), 50);
  EXPECT_TRUE(book().bids().empty());
}

TEST_F(ExchangeFixture, SweepStopsAtLimitPrice) {
  limit(Side::Sell, 101, 10, kBob);
  limit(Side::Sell, 110, 90, kBob);

  // Buyer will pay at most 105, so the 110 level is untouchable; residual rests.
  limit(Side::Buy, 105, 100, kAlice);

  EXPECT_EQ(traded_qty(), 10);
  EXPECT_EQ(best_bid(), 105);
  EXPECT_EQ(bid_qty(), 90);
  EXPECT_EQ(best_ask(), 110);
}

TEST_F(ExchangeFixture, TimePriorityWithinLevel) {
  const OrderId first = limit(Side::Sell, 100, 10, kBob);
  const OrderId second = limit(Side::Sell, 100, 10, kCarol);

  limit(Side::Buy, 100, 10, kAlice);

  // The earlier resting order must fill, not the later one.
  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting_id, first);
  EXPECT_FALSE(order_live(first));
  EXPECT_TRUE(order_live(second));
}

TEST_F(ExchangeFixture, SellAggressorHitsHighestBid) {
  limit(Side::Buy, 98, 10, kBob);
  limit(Side::Buy, 100, 10, kCarol);
  limit(Side::Buy, 99, 10, kBob);

  limit(Side::Sell, 98, 10, kAlice);

  const auto t = trade_events();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].trade_price, 100); // best bid, not the aggressor's limit
  EXPECT_EQ(best_bid(), 99);
}

TEST_F(ExchangeFixture, AggressorSideRecordedOnTrade) {
  limit(Side::Sell, 100, 10, kBob);
  limit(Side::Buy, 100, 10, kAlice);
  ASSERT_EQ(trades(), 1u);
  EXPECT_EQ(trade_events()[0].aggressor_side, Aggressor::Buy);

  limit(Side::Buy, 100, 10, kBob);
  limit(Side::Sell, 100, 10, kAlice);
  ASSERT_EQ(trades(), 1u);
  EXPECT_EQ(trade_events()[0].aggressor_side, Aggressor::Sell);
}

// --- market orders ---------------------------------------------------------

TEST_F(ExchangeFixture, MarketOrderSweepsBook) {
  limit(Side::Sell, 101, 30, kBob);
  limit(Side::Sell, 102, 30, kBob);

  market(Side::Buy, 50, kAlice);

  EXPECT_EQ(traded_qty(), 50);
  EXPECT_EQ(best_ask(), 102);
  EXPECT_EQ(ask_qty(), 10);
}

TEST_F(ExchangeFixture, MarketOrderNeverRests) {
  limit(Side::Sell, 101, 20, kBob);

  market(Side::Buy, 100, kAlice);

  EXPECT_EQ(traded_qty(), 20);
  // Residual 80 is cancelled rather than resting; the book is empty.
  EXPECT_TRUE(book().empty());
  ASSERT_EQ(cancels(), 1u);
  const Event *c = buf.first(EventType::OrderCancelled);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->cancel_cause, CancelCause::IocResidual);
  EXPECT_EQ(c->remaining, 80);
}

TEST_F(ExchangeFixture, MarketOrderIntoEmptyBookIsRejected) {
  market(Side::Buy, 10, kAlice);
  EXPECT_EQ(trades(), 0u);
  ASSERT_EQ(rejects(), 1u);
  EXPECT_EQ(reject_reason(), RejectReason::NoLiquidity);
}

TEST_F(ExchangeFixture, MarketOrderIgnoresOppositeSideDepthOnly) {
  // Liquidity exists, but on the same side as the market order.
  limit(Side::Buy, 100, 50, kBob);
  market(Side::Buy, 10, kAlice);
  EXPECT_EQ(reject_reason(), RejectReason::NoLiquidity);
}

// --- time in force ---------------------------------------------------------

TEST_F(ExchangeFixture, IocCancelsResidual) {
  limit(Side::Sell, 100, 30, kBob);
  limit(Side::Buy, 100, 100, kAlice, TimeInForce::Ioc);

  EXPECT_EQ(traded_qty(), 30);
  EXPECT_TRUE(book().bids().empty());
  const Event *c = buf.first(EventType::OrderCancelled);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->cancel_cause, CancelCause::IocResidual);
  EXPECT_EQ(c->remaining, 70);
}

TEST_F(ExchangeFixture, IocWithNoLiquidityFullyCancelled) {
  limit(Side::Buy, 100, 50, kAlice, TimeInForce::Ioc);
  EXPECT_EQ(trades(), 0u);
  EXPECT_TRUE(book().empty());
  EXPECT_EQ(cancels(), 1u);
}

TEST_F(ExchangeFixture, FokFillsCompletelyOrRejects) {
  limit(Side::Sell, 100, 40, kBob);

  // 100 requested, only 40 available => reject, and no trade occurs.
  limit(Side::Buy, 100, 100, kAlice, TimeInForce::Fok);
  EXPECT_EQ(trades(), 0u);
  ASSERT_EQ(rejects(), 1u);
  EXPECT_EQ(reject_reason(), RejectReason::FillOrKillUnfillable);
  // The resting order must be untouched.
  EXPECT_EQ(ask_qty(), 40);

  // Now request exactly what is available.
  limit(Side::Buy, 100, 40, kAlice, TimeInForce::Fok);
  EXPECT_EQ(traded_qty(), 40);
  EXPECT_TRUE(book().empty());
}

TEST_F(ExchangeFixture, FokAcrossMultipleLevels) {
  limit(Side::Sell, 100, 30, kBob);
  limit(Side::Sell, 101, 30, kCarol);

  limit(Side::Buy, 101, 60, kAlice, TimeInForce::Fok);
  EXPECT_EQ(traded_qty(), 60);
  EXPECT_TRUE(book().empty());
}

TEST_F(ExchangeFixture, FokIgnoresLiquidityBeyondItsLimit) {
  limit(Side::Sell, 100, 30, kBob);
  limit(Side::Sell, 200, 100, kCarol);

  // Only the 100 level is reachable at limit 100, so 60 is unfillable.
  limit(Side::Buy, 100, 60, kAlice, TimeInForce::Fok);
  EXPECT_EQ(reject_reason(), RejectReason::FillOrKillUnfillable);
  EXPECT_EQ(trades(), 0u);
}

// --- quantity conservation -------------------------------------------------

TEST_F(ExchangeFixture, TradeQuantityMatchesBookReduction) {
  limit(Side::Sell, 100, 100, kBob);
  const Qty before = ask_qty();

  limit(Side::Buy, 100, 35, kAlice);

  EXPECT_EQ(traded_qty(), 35);
  EXPECT_EQ(before - ask_qty(), 35);
}

TEST_F(ExchangeFixture, CrossedBookNeverPersists) {
  limit(Side::Buy, 105, 10, kAlice);
  limit(Side::Sell, 100, 10, kBob);
  // A crossing pair must trade, never rest crossed.
  EXPECT_EQ(trades(), 1u);
  EXPECT_TRUE(book().check_invariants());
}
