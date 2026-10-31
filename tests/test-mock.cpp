#include <algotradelib/backtest>
#include <algotradelib/live>
#include <algotradelib/mock>

#include "live_program.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace algotradelib;

namespace {

using Clock = std::chrono::system_clock;

/** An instant in UTC. */
Clock::time_point utc(int year, int month, int day, int hour, int minute, int second = 0) {
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days = 365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  return Clock::time_point{} + std::chrono::hours((era_days - 719468) * 24 + hour) +
    std::chrono::minutes(minute) + std::chrono::seconds(second);
}

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

Decimal64 dec(const char* text) { return Decimal64::parse(text); }

/** One bar; `open` empty when nothing traded at the open. */
Bar bar(Clock::time_point at, const char* open, const char* high, const char* low, const char* close) {
  Bar made;
  made.timestamp_ = at;
  made.open_ = dec(open);
  made.high_ = dec(high);
  made.low_ = dec(low);
  made.close_ = dec(close);
  made.volume_ = dec("1000");
  return made;
}

/** Close-stamped daily bars of 005930 at 15:30 KST (06:30 UTC) on 2026-03-02 (a Monday) and the following days. */
BarSeries daily(std::vector<Bar> bars) {
  BarSeries series;
  series.instrument_ = samsung();
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::CLOSE;
  series.currency_ = Currency{"KRW"};
  series.bars_ = std::move(bars);
  return series;
}

/** Three days: 100 to 110, 105 to 115, 95 to 105. */
BarSeries three_days() {
  return daily({
    bar(utc(2026, 3, 2, 6, 30), "100", "110", "100", "105"),
    bar(utc(2026, 3, 3, 6, 30), "106", "115", "105", "110"),
    bar(utc(2026, 3, 4, 6, 30), "100", "105", "95", "98"),
  });
}

mock::MarketSettings cash(const char* amount, Clock::time_point start) {
  mock::MarketSettings settings;
  settings.cash_ = Money{dec(amount), Currency{"KRW"}};
  settings.start_ = start;
  return settings;
}

OrderRequest limit_buy(const char* quantity, const char* price) {
  OrderRequest order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = dec(quantity);
  order.price_ = dec(price);
  return order;
}

OrderRequest market_order(OrderSide side, const char* quantity) {
  OrderRequest order;
  order.instrument_ = samsung();
  order.side_ = side;
  order.type_ = OrderType::MARKET;
  order.quantity_ = dec(quantity);
  return order;
}

std::string code_of(const BrokerError& error) { return error.broker_code_; }

const OrderSnapshot& only_order(const mock::Market& market) {
  static OrderPage page;
  page = market.order_history({});
  REQUIRE(page.orders_.size() == 1);
  return page.orders_.front();
}

} // namespace

TEST_CASE("the mock market answers only bars that have ended", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 3, 6, 0)));
  BarQuery query;
  query.instrument_ = samsung();
  query.count_ = 10;
  auto read = market.bars(query);
  REQUIRE(read);
  REQUIRE(read.value().bars_.size() == 1);
  CHECK(read.value().received_at_ == utc(2026, 3, 3, 6, 0));

  market.advance_to(utc(2026, 3, 3, 6, 30));
  read = market.bars(query);
  REQUIRE(read.value().bars_.size() == 2);
  query.count_ = 1;
  read = market.bars(query);
  REQUIRE(read.value().bars_.size() == 1);
  CHECK(read.value().bars_.front().close_ == dec("110"));
  query.before_ = utc(2026, 3, 2, 6, 30);
  read = market.bars(query);
  REQUIRE(read.value().bars_.size() == 1);
  CHECK(read.value().bars_.front().close_ == dec("105"));

  query.count_ = 0;
  CHECK(code_of(market.bars(query).error()) == "bars_count");
  query.count_ = 1;
  query.interval_ = BarInterval::ONE_MINUTE;
  CHECK(code_of(market.bars(query).error()) == "unsupported_interval");
  query.instrument_.symbol_ = "000660";
  CHECK(code_of(market.bars(query).error()) == "unknown_instrument");
}

TEST_CASE("the mock quote is the last closed bar", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 1, 0, 0)));
  const auto none = market.quote(samsung());
  REQUIRE_FALSE(none);
  CHECK(none.error().category_ == ErrorCategory::PROTOCOL);
  CHECK(code_of(none.error()) == "no_price");
  market.advance_to(utc(2026, 3, 2, 7, 0));
  const auto quote = market.quote(samsung());
  REQUIRE(quote);
  CHECK(quote.value().last_price_ == dec("105"));
  CHECK(quote.value().exchange_time_ == utc(2026, 3, 2, 6, 30));
  CHECK(code_of(market.quote(InstrumentId{Country::KR, Venue::COMBINED, "1"}).error()) == "unknown_instrument");
}

TEST_CASE("the mock market refuses an order in its documented order", "[mock]") {
  mock::Market market({three_days()}, cash("1000", utc(2026, 3, 1, 0, 0)));
  auto order = limit_buy("1", "100");
  order.instrument_.symbol_ = "000660";
  CHECK(code_of(market.place(order).error()) == "unknown_instrument");
  order = limit_buy("1", "100");
  order.type_ = OrderType::STOP_LIMIT;
  CHECK(code_of(market.place(order).error()) == "unsupported_order_type");
  order = limit_buy("1", "100");
  order.time_in_force_ = TimeInForce::UNKNOWN;
  CHECK(code_of(market.place(order).error()) == "unsupported_time_in_force");
  CHECK(code_of(market.place(limit_buy("1.5", "100")).error()) == "invalid_quantity");
  CHECK(code_of(market.place(limit_buy("0", "100")).error()) == "invalid_quantity");
  CHECK(code_of(market.place(limit_buy("1", "0")).error()) == "invalid_price");
  order = market_order(OrderSide::BUY, "1");
  order.price_ = dec("100");
  CHECK(code_of(market.place(order).error()) == "invalid_price");
  CHECK(code_of(market.place(market_order(OrderSide::BUY, "1")).error()) == "no_price");
  CHECK(code_of(market.place(limit_buy("11", "100")).error()) == "insufficient_cash");
  CHECK(code_of(market.place(market_order(OrderSide::SELL, "1")).error()) == "insufficient_holdings");
  const auto refused = market.place(limit_buy("11", "100"));
  CHECK(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  CHECK(refused.error().reached_broker_);
  CHECK(market.order_history({}).orders_.empty());
}

TEST_CASE("a mock market order fills at the open of the next bar", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  const auto placed = market.place(market_order(OrderSide::BUY, "10"));
  REQUIRE(placed);
  CHECK(placed.value().ack_.order_id_ == "1");
  CHECK(placed.value().ref_.primary_ == "1");
  REQUIRE(market.working_orders().orders_.size() == 1);
  // Reserved at the last close, 105.
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("998950"));

  market.advance_to(utc(2026, 3, 3, 6, 29));
  CHECK(market.holdings().positions_.empty());
  market.advance_to(utc(2026, 3, 3, 6, 30));
  const auto& filled = only_order(market);
  CHECK(filled.status_ == OrderStatus::FILLED);
  CHECK(filled.execution_.average_filled_price_ == dec("106"));
  CHECK(filled.execution_.filled_at_ == utc(2026, 3, 3, 6, 30));
  CHECK(filled.ordered_at_ == utc(2026, 3, 2, 6, 30));
  CHECK(market.working_orders().orders_.empty());
  const auto holdings = market.holdings();
  REQUIRE(holdings.positions_.size() == 1);
  CHECK(holdings.positions_.front().quantity_ == dec("10"));
  CHECK(holdings.positions_.front().sellable_quantity_ == dec("10"));
  CHECK(holdings.positions_.front().last_price_ == dec("110"));
  CHECK(holdings.positions_.front().unrealized_profit_loss_.amount_ == dec("40"));
  REQUIRE(holdings.totals_.size() == 1);
  CHECK(holdings.totals_.front().market_value_ == dec("1100"));
  CHECK(holdings.sellable_basis_.coverage_ == SellableBasis::Coverage::WHOLE_HOLDING);
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("998940"));

  // Sold back; the flat position leaves the holdings.
  REQUIRE(market.place(market_order(OrderSide::SELL, "10")));
  CHECK(code_of(market.place(market_order(OrderSide::SELL, "1")).error()) == "insufficient_holdings");
  market.advance_to(utc(2026, 3, 4, 6, 30));
  CHECK(market.holdings().positions_.empty());
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("999940"));
}

TEST_CASE("a mock limit fills only when its bar reaches it and a day order expires after its session", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  REQUIRE(market.place(limit_buy("1", "104")));  // day 2's low is 105
  REQUIRE(market.place(limit_buy("1", "108")));  // reached: filled at the limit
  market.advance_to(utc(2026, 3, 3, 6, 30));
  const auto page = market.order_history({});
  REQUIRE(page.orders_.size() == 2);
  CHECK(page.orders_[0].status_ == OrderStatus::CANCELED);
  CHECK(page.orders_[0].canceled_at_ == utc(2026, 3, 3, 6, 30));
  CHECK(page.orders_[1].status_ == OrderStatus::FILLED);
  CHECK(page.orders_[1].execution_.average_filled_price_ == dec("108"));
  CHECK(market.working_orders().orders_.empty());
}

TEST_CASE("an immediate-or-cancel order lives for one bar", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  auto order = limit_buy("1", "100");
  order.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  REQUIRE(market.place(order));
  market.advance_to(utc(2026, 3, 3, 6, 30));
  CHECK(only_order(market).status_ == OrderStatus::CANCELED);
}

namespace {

/** Close-stamped minute bars of 005930 on 2026-03-02 KST, ending at each given minute after 09:00. */
BarSeries minutes(std::vector<std::pair<int, int>> ends, Clock::time_point (*day)(int, int)) {
  BarSeries series;
  series.instrument_ = samsung();
  series.interval_ = BarInterval::ONE_MINUTE;
  series.stamp_ = BarStamp::CLOSE;
  series.currency_ = Currency{"KRW"};
  int price = 100;
  for (const auto& end : ends) {
    const auto text = std::to_string(price);
    series.bars_.push_back(bar(day(end.first, end.second), text.c_str(), text.c_str(), text.c_str(), text.c_str()));
    ++price;
  }
  return series;
}

/** A KST time on 2026-03-02, or with an hour of 24 or more, on the next day. */
Clock::time_point kst(int hour, int minute) { return utc(2026, 3, 2, 0, 0) + std::chrono::hours(hour - 9) + std::chrono::minutes(minute); }

} // namespace

TEST_CASE("a mock order never fills against a bar that began before it was accepted", "[mock]") {
  // Bars end at 09:01, 09:02 and 09:03, opening at 100, 101 and 102.
  mock::Market market({minutes({{9, 1}, {9, 2}, {9, 3}}, kst)}, cash("1000000", kst(9, 1)));
  market.advance_to(kst(9, 1) + std::chrono::seconds(1));
  REQUIRE(market.place(market_order(OrderSide::BUY, "1")));
  market.advance_to(kst(9, 2));
  CHECK(only_order(market).status_ == OrderStatus::PENDING);  // the 09:01-09:02 bar began before it
  market.advance_to(kst(9, 3));
  CHECK(only_order(market).execution_.average_filled_price_ == dec("102"));
}

TEST_CASE("a mock intraday day order expires at the close or before a later date", "[mock]") {
  SECTION("a bar ending at the close ends its session") {
    mock::Market market({minutes({{15, 29}, {15, 30}}, kst)}, cash("1000000", kst(15, 0)));
    REQUIRE(market.place(limit_buy("1", "50")));
    market.advance_to(kst(15, 29));
    CHECK(only_order(market).status_ == OrderStatus::PENDING);
    market.advance_to(kst(15, 30));
    CHECK(only_order(market).status_ == OrderStatus::CANCELED);
  }
  SECTION("without a closing bar it works until a bar of a later date") {
    // The data's last bar of the day ends at noon; the next is the following morning's.
    mock::Market market({minutes({{11, 59}, {12, 0}, {33, 1}}, kst)}, cash("1000000", kst(11, 0)));
    REQUIRE(market.place(limit_buy("1", "50")));
    market.advance_to(kst(20, 0));
    CHECK(only_order(market).status_ == OrderStatus::PENDING);
    market.advance_to(kst(33, 1));
    CHECK(only_order(market).status_ == OrderStatus::CANCELED);
  }
}

TEST_CASE("the mock market charges the fill model's commission and tax", "[mock]") {
  auto settings = cash("1000000", utc(2026, 3, 2, 6, 30));
  settings.fills_ = backtest::NextBarFillModel(backtest::CommissionModel::of(dec("0.01")))
    .with_sell_tax(backtest::SellTax{dec("0.002"), Country::KR});
  mock::Market market({three_days()}, settings);
  REQUIRE(market.place(market_order(OrderSide::BUY, "10")));
  market.advance_to(utc(2026, 3, 3, 6, 30));  // 10 at 106: 1,060 and 10 commission
  REQUIRE(market.place(market_order(OrderSide::SELL, "10")));
  market.advance_to(utc(2026, 3, 4, 6, 30));  // 10 at 100: 1,000, 10 commission and 2 tax
  const auto page = market.order_history({});
  REQUIRE(page.orders_.size() == 2);
  CHECK(page.orders_[0].execution_.commission_->amount_ == dec("10"));
  CHECK(page.orders_[1].execution_.commission_->amount_ == dec("10"));
  CHECK(page.orders_[1].execution_.tax_->amount_ == dec("2"));
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("999918"));
}

TEST_CASE("mock buying power reserves open buys", "[mock]") {
  mock::Market market({three_days()}, cash("1000", utc(2026, 3, 2, 6, 30)));
  REQUIRE(market.place(limit_buy("6", "100")));
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("400"));
  CHECK(code_of(market.place(limit_buy("5", "100")).error()) == "insufficient_cash");
  CHECK(code_of(market.buying_power(Currency{"USD"}).error()) == "unknown_currency");
  REQUIRE(market.cancel("1"));
  CHECK(only_order(market).status_ == OrderStatus::CANCELED);
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("1000"));
  CHECK(code_of(market.cancel("1").error()) == "nothing_to_cancel");
  CHECK(code_of(market.cancel("9").error()) == "nothing_to_cancel");
  CHECK(market.cancel("9").error().reached_broker_);
}

TEST_CASE("the mock history keeps orders accepted on the dates asked", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  REQUIRE(market.place(limit_buy("1", "90")));
  market.advance_to(utc(2026, 3, 3, 6, 30));
  REQUIRE(market.place(limit_buy("1", "90")));
  OrderHistoryQuery query;
  query.from_ = Date{2026, 3, 3};
  query.to_ = Date{2026, 3, 3};
  const auto page = market.order_history(query);
  REQUIRE(page.orders_.size() == 1);
  CHECK(page.orders_.front().order_id_ == "2");
  CHECK_FALSE(page.next_cursor_);
}

TEST_CASE("the mock market refuses what it cannot model", "[mock]") {
  auto weekly = three_days();
  weekly.interval_ = BarInterval::ONE_WEEK;
  CHECK_THROWS_AS(mock::Market({weekly}, cash("1", {})), std::invalid_argument);
  auto backwards = three_days();
  std::swap(backwards.bars_[0], backwards.bars_[1]);
  CHECK_THROWS_AS(mock::Market({backwards}, cash("1", {})), std::invalid_argument);
  CHECK_THROWS_AS(mock::Market({three_days(), three_days()}, cash("1", {})), std::invalid_argument);
  CHECK_THROWS_AS(mock::Market({three_days()}, cash("-1", {})), std::invalid_argument);
  Instrument other;
  other.id_ = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  CHECK_THROWS_AS(mock::Market({three_days()}, cash("1", {}), {other}), std::invalid_argument);
  mock::Market market({three_days()}, cash("1", utc(2026, 3, 2, 0, 0)));
  CHECK_THROWS_AS(market.advance_to(utc(2026, 3, 1, 0, 0)), std::invalid_argument);
}

TEST_CASE("the mock market answers the reference data it was given or its default", "[mock]") {
  Instrument given;
  given.id_ = samsung();
  given.name_ = "Samsung";
  given.lot_size_ = dec("10");
  given.price_rule_ = PriceRule::krx_equity();
  mock::Market market({three_days()}, cash("1", {}), {given});
  CHECK(market.instrument(samsung()).value().lot_size_ == dec("10"));
  mock::Market plain({three_days()}, cash("1", {}));
  const auto made = plain.instrument(samsung());
  REQUIRE(made);
  CHECK(made.value().lot_size_ == dec("1"));
  CHECK(made.value().currency_ == Currency{"KRW"});
  CHECK(made.value().price_rule_.kind_ == PriceRule::Kind::UNCONSTRAINED);
  CHECK(code_of(plain.instrument(InstrumentId{}).error()) == "unknown_instrument");
}

TEST_CASE("the mock market halts on a fill it cannot cost or record", "[mock]") {
  SECTION("a commission schedule with no rate for the fill's date") {
    CommissionRate rate;
    rate.country_ = Country::KR;
    rate.venue_ = *VenueKey::normalized(Venue::KRX);
    rate.rate_ = dec("0.001");
    rate.institution_rate_ = dec("0");
    rate.until_ = Date{2026, 3, 2};
    CommissionSchedule schedule;
    schedule.origin_ = CommissionOrigin::CALLER_STATED;
    schedule.tariff_ = "test tariff";
    schedule.rates_ = {rate};
    auto policy = backtest::CommissionPolicy::scheduled(schedule, *VenueKey::normalized(Venue::KRX), Country::KR);
    REQUIRE(policy);
    auto settings = cash("1000000", utc(2026, 3, 2, 6, 30));
    settings.fills_ = backtest::NextBarFillModel(std::move(policy.value()));
    mock::Market market({three_days()}, settings);
    REQUIRE(market.place(market_order(OrderSide::BUY, "1")));
    CHECK_THROWS_AS(market.advance_to(utc(2026, 3, 3, 6, 30)), std::runtime_error);
    REQUIRE(market.halted());
    CHECK(market.now() == utc(2026, 3, 2, 6, 30));
    CHECK(only_order(market).status_ == OrderStatus::PENDING);
    CHECK_THROWS_AS(market.advance_to(utc(2026, 3, 4, 6, 30)), std::runtime_error);
    CHECK(code_of(market.place(market_order(OrderSide::BUY, "1")).error()) == "market_halted");
  }
  SECTION("a fill amount that does not fit an exact decimal") {
    auto huge = daily({
      bar(utc(2026, 3, 2, 6, 30), "1", "1", "1", "1"),
      bar(utc(2026, 3, 3, 6, 30), "1000000000000000000", "1000000000000000000", "1", "1"),
    });
    mock::Market market({huge}, cash("100", utc(2026, 3, 2, 6, 30)));
    REQUIRE(market.place(market_order(OrderSide::BUY, "10")));
    CHECK_THROWS_AS(market.advance_to(utc(2026, 3, 3, 6, 30)), std::runtime_error);
    CHECK(only_order(market).status_ == OrderStatus::PENDING);
    CHECK(market.holdings().positions_.empty());
  }
}

namespace {

/** A session over a mock market. */
struct MockSession {
  explicit MockSession(mock::Market& market)
  : client_(std::make_shared<mock::MockClient>(market)), session_(client_, mock::MockAccount{}) {}

  std::shared_ptr<mock::MockClient> client_;
  BlockingSession<mock::MockClient> session_;
};

BrokerError transport() {
  BrokerError error;
  error.category_ = ErrorCategory::TRANSPORT;
  error.reached_broker_ = true;
  return error;
}

} // namespace

TEST_CASE("the mock broker is a paper broker bound like any other", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  MockSession bound(market);
  CHECK(bound.session_.client().environment() == "paper");
  CHECK(mock::environment == "paper");
  CHECK(BrokerTraits<mock::MockClient>::name == "mock");
  CHECK(bound.session_.rate_limit_stats() == nullptr);
  CHECK(bound.session_.quote(samsung()).value().last_price_ == dec("105"));
  CHECK(bound.session_.instrument(samsung()));

  // Refused before the client: a trigger price, an idempotency key, a modification, a partial
  // cancellation, a filtered history, an interval no market holds.
  auto keyed = limit_buy("1", "100");
  keyed.client_order_id_ = "k1";
  CHECK(code_of(bound.session_.place(keyed).error()) == "idempotency_key_unsupported");
  auto stop = limit_buy("1", "100");
  stop.type_ = OrderType::STOP_LIMIT;
  stop.trigger_price_ = dec("99");
  CHECK_FALSE(bound.session_.place(stop));
  const auto placed = bound.session_.place(limit_buy("1", "100"));
  REQUIRE(placed);
  CHECK_FALSE(bound.session_.replace(placed.value().ref_, AmendInstruction{}).error().reached_broker_);
  AmendInstruction part;
  part.quantity_ = dec("1");
  CHECK(code_of(bound.session_.cancel(placed.value().ref_, part).error()) == "partial_cancel_unsupported");
  OrderHistoryQuery filtered;
  filtered.symbol_ = "005930";
  CHECK(code_of(bound.session_.order_history(filtered).error()) == "history_filter_unsupported");
  BarQuery weekly;
  weekly.instrument_ = samsung();
  weekly.interval_ = BarInterval::ONE_WEEK;
  CHECK(code_of(bound.session_.bars(weekly).error()) == "unsupported_interval");
  REQUIRE(bound.session_.working_orders().value().orders_.size() == 1);
  AmendInstruction all;
  all.all_remaining_ = true;
  REQUIRE(bound.session_.cancel(placed.value().ref_, all));
  CHECK(bound.session_.working_orders().value().orders_.empty());
  CHECK(bound.session_.holdings().value().positions_.empty());
  CHECK(bound.session_.buying_power(Currency{"KRW"}));
}

TEST_CASE("mock faults answer the calls they are scripted for", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  mock::MockClient client(market);

  SECTION("a failed call is not carried out") {
    client.fail_next(mock::Call::PLACE, transport());
    const auto failed = client.place(limit_buy("1", "100"));
    REQUIRE_FALSE(failed);
    CHECK(failed.error().category_ == ErrorCategory::TRANSPORT);
    CHECK(market.order_history({}).orders_.empty());
    CHECK(client.place(limit_buy("1", "100")));
    client.fail_next(mock::Call::WORKING_ORDERS, transport(), 2);
    CHECK_FALSE(client.working_orders());
    CHECK_FALSE(client.working_orders());
    CHECK(client.working_orders());
    client.fail_next(mock::Call::QUOTE, transport());
    client.fail_next(mock::Call::BARS, transport());
    client.fail_next(mock::Call::INSTRUMENT, transport());
    client.fail_next(mock::Call::HOLDINGS, transport());
    client.fail_next(mock::Call::BUYING_POWER, transport());
    client.fail_next(mock::Call::CANCEL, transport());
    client.fail_next(mock::Call::ORDER_HISTORY, transport());
    CHECK_FALSE(client.quote(samsung()));
    BarQuery query;
    query.instrument_ = samsung();
    CHECK_FALSE(client.bars(query));
    CHECK_FALSE(client.instrument(samsung()));
    CHECK_FALSE(client.holdings());
    CHECK_FALSE(client.buying_power(Currency{"KRW"}));
    CHECK_FALSE(client.cancel(mock::MockAmendment{"1"}));
    CHECK_FALSE(client.order_history({}));
    CHECK(market.working_orders().orders_.size() == 1);
    CHECK_THROWS_AS(client.fail_next(mock::Call::PLACE, transport(), 0), std::invalid_argument);
  }

  SECTION("a lost reply leaves the order in the market") {
    client.lose_next_reply(mock::Call::PLACE);
    const auto lost = client.place(limit_buy("1", "100"));
    REQUIRE_FALSE(lost);
    CHECK(lost.error().category_ == ErrorCategory::TRANSPORT);
    CHECK(lost.error().reached_broker_);
    REQUIRE(client.working_orders().value().orders_.size() == 1);
    client.lose_next_reply(mock::Call::CANCEL);
    CHECK_FALSE(client.cancel(mock::MockAmendment{"1"}));
    CHECK(client.working_orders().value().orders_.empty());
    CHECK_THROWS_AS(client.lose_next_reply(mock::Call::HOLDINGS), std::invalid_argument);
  }

  SECTION("a repeated read hides a fill the market has made") {
    REQUIRE(client.place(market_order(OrderSide::BUY, "10")));
    REQUIRE(client.working_orders().value().orders_.size() == 1);
    REQUIRE(client.holdings().value().positions_.empty());
    market.advance_to(utc(2026, 3, 3, 6, 30));
    client.repeat_last(mock::Call::WORKING_ORDERS);
    client.repeat_last(mock::Call::HOLDINGS);
    const auto stale = client.working_orders();
    REQUIRE(stale.value().orders_.size() == 1);
    CHECK(stale.value().received_at_ == utc(2026, 3, 3, 6, 30));
    CHECK(client.holdings().value().positions_.empty());
    CHECK(code_of(client.cancel(mock::MockAmendment{"1"}).error()) == "nothing_to_cancel");
    CHECK(client.working_orders().value().orders_.empty());
    CHECK(client.holdings().value().positions_.size() == 1);
    CHECK_THROWS_AS(client.repeat_last(mock::Call::PLACE), std::invalid_argument);
    CHECK_THROWS_AS(client.repeat_last(mock::Call::HOLDINGS, 0), std::invalid_argument);
    // With no earlier answer to repeat, a read answers normally.
    mock::MockClient fresh(market);
    fresh.repeat_last(mock::Call::ORDER_HISTORY);
    CHECK(fresh.order_history({}).value().orders_.size() == 1);
  }
}

TEST_CASE("the mock clock moves the market as a loop waits", "[mock]") {
  mock::Market market({three_days()}, cash("1000000", utc(2026, 3, 2, 6, 30)));
  mock::MarketClock clock(market);
  const auto before = clock.monotonic_now();
  REQUIRE(market.place(market_order(OrderSide::BUY, "1")));
  clock.wait_for(std::chrono::hours(-1));
  CHECK(clock.now() == utc(2026, 3, 2, 6, 30));
  clock.wait_for(std::chrono::hours(24));
  CHECK(clock.now() == utc(2026, 3, 3, 6, 30));
  CHECK(before < clock.monotonic_now());
  CHECK(only_order(market).status_ == OrderStatus::FILLED);
}

namespace {

/** Holds 10 shares on the first three days of each six, nothing on the others, with limits 2% through the close. */
struct Flips {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& event) {
    const auto day = std::chrono::duration_cast<std::chrono::hours>(event.bar_.timestamp_.time_since_epoch()).count() / 24;
    const bool hold = (day / 3) % 2 == 0;
    // A target of 10 can only buy, a target of 0 only sell.
    const auto factor = Decimal64::parse(hold ? "1.02" : "0.98");
    const auto price = decimal_divide(*decimal_multiply(event.bar_.close_, factor), Decimal64::parse("1"), 2);
    strategy::TargetPosition target;
    target.instrument_ = event.instrument_;
    target.target_quantity_ = Decimal64::parse(hold ? "10" : "0");
    target.limit_ = strategy::Price::checked(*price, PriceRule::unconstrained());
    target.tag_ = "flips";
    ctx.submit(target);
  }
};

/** Daily bars on 40 weekdays from 2026-03-02, a deterministic walk that gaps up to 3.6% at the open, so some limits 2% through the close miss. */
BarSeries forty_days() {
  BarSeries series = daily({});
  auto day = utc(2026, 3, 2, 6, 30);
  long close = 10000;
  for (int k = 0; series.bars_.size() < 40; ++k, day += std::chrono::hours(24)) {
    const auto weekday = (std::chrono::duration_cast<std::chrono::hours>(day.time_since_epoch()).count() / 24 + 4) % 7;
    if (weekday == 0 || weekday == 6) {
      continue;
    }
    const long open = close + ((k * 37) % 7 - 3) * 120;
    close = open + ((k * 53) % 9 - 4) * 30;
    const long high = std::max(open, close) + 100;
    const long low = std::min(open, close) - 100;
    series.bars_.push_back(bar(
      day, std::to_string(open).c_str(), std::to_string(high).c_str(), std::to_string(low).c_str(),
      std::to_string(close).c_str()
    ));
  }
  return series;
}

/** A file path in the temporary directory, removed on destruction. */
struct ScratchFile {
  explicit ScratchFile(const char* name) : path_((std::filesystem::temp_directory_path() / name).string()) {
    std::remove(path_.c_str());
  }
  ~ScratchFile() { std::remove(path_.c_str()); }
  std::string path_;
};

} // namespace

TEST_CASE("a live run on the mock trades as a backtest of the same bars", "[mock][acceptance]") {
  // The fixture the parity holds in: close-stamped daily bars, a poll on every close (a 15-minute
  // cadence from a start at a close), the run starting just after the first close so nothing is
  // warm-up, a strategy that decides from the bar alone, and the same reference data, cash, fill
  // model and limits.
  const auto bars = forty_days();
  Instrument reference;
  reference.id_ = samsung();
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = dec("1");
  reference.price_rule_ = PriceRule::unconstrained();
  strategy::RiskLimits limits;
  limits.permitted_phases_ = std::nullopt;

  backtest::ReplayConfig config;
  config.cash_ = dec("1000000");
  config.instruments_ = {reference};
  config.limits_ = limits;
  backtest::Backtester<Flips> replay(Flips{}, config);
  for (const auto& one : bars.bars_) {
    replay.feed(strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, one));
  }
  replay.finish();
  const auto& replayed = replay.result().executions_;
  REQUIRE(replayed.size() >= 6);

  mock::Market market({bars}, cash("1000000", bars.bars_.front().timestamp_), {reference});
  MockSession bound(market);
  const ScratchFile journal("atl-test-mock-acceptance.jsonl");
  const ScratchFile control("atl-test-mock-acceptance.stop");
  live_program::Settings settings;
  settings.universe_ = {samsung()};
  settings.limits_ = limits;
  settings.schedule_.tick_ = std::chrono::minutes(15);
  settings.schedule_.bars_ = std::chrono::minutes(15);
  settings.schedule_.holdings_ = std::chrono::minutes(15);
  settings.schedule_.working_orders_ = std::chrono::minutes(15);
  settings.schedule_.calendar_.reset();
  settings.bar_count_ = 5;
  settings.maximum_portfolio_age_ = std::chrono::minutes(30);
  settings.journal_path_ = journal.path_;
  settings.journal_scope_ = "mock:paper:1";
  settings.control_file_ = control.path_;
  const auto span = bars.bars_.back().timestamp_ - bars.bars_.front().timestamp_;
  settings.max_iterations_ = static_cast<std::size_t>(span / std::chrono::minutes(15)) + 2;
  runner::StopSource stop;
  std::ostringstream out;
  const auto exit = live_program::trade(
    bound.session_, bound.session_, Flips{}, settings, stop, out, mock::MarketClock(market)
  );
  INFO(out.str());
  REQUIRE(exit == live_program::Exit::CLEAN);

  std::vector<OrderSnapshot> filled;
  for (const auto& order : market.order_history({}).orders_) {
    if (order.status_ == OrderStatus::FILLED) {
      filled.push_back(order);
    }
  }
  // Some limits missed and expired, and were placed again on a later bar.
  CHECK(market.order_history({}).orders_.size() > filled.size());
  REQUIRE(filled.size() == replayed.size());
  for (std::size_t k = 0; k < filled.size(); ++k) {
    CAPTURE(k);
    CHECK(filled[k].side_ == replayed[k].fill_.side_);
    CHECK(filled[k].execution_.filled_quantity_ == replayed[k].fill_.quantity_);
    CHECK(filled[k].execution_.average_filled_price_ == replayed[k].fill_.price_);
    CHECK(filled[k].execution_.filled_at_ == replayed[k].fill_.at_);
  }
}
