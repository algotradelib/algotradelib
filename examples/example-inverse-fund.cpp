// A bearish view on an index, held as an ordinary long position in an inverse fund.
//
// No adapter this library ships can sell what the account does not hold: KIS, Kiwoom and Toss take cash
// orders. A target below zero there is refused by the guard before it is sent
// (`RiskLimits::enforce_sellable_quantity_`, on by default). A replay can still simulate a short by
// turning that check off where the replay is configured; that models no borrow, and nothing live.
//
// What a cash account can do is **buy a fund that moves against an index**. This example does exactly
// that and nothing cleverer: the strategy reads one instrument for its signal and states a target for
// another, the fund, by the fund's own identity. Nothing is translated, aliased or substituted behind
// the strategy's back: the order, the fill, the holding and the exit are the fund's.
//
// What this is not, so the output is not read as more than it is:
//
//  - It is not a short of the index, and it cannot stand in for a short of one stock. The fund targets
//    the opposite of its benchmark's **daily** return and resets every day, so over weeks its value
//    parts from what a short opened on day one would be worth, in either direction.
//  - It spends cash to buy fund shares. A short credits cash and owes shares, with a borrow fee, a
//    recall risk and margin. None of that is here.
//  - It carries the fund's own costs and tracking difference, and its benchmark is a futures index.
//  - Whether an account may buy a given fund (some need an agreement or a deposit) is the broker's to
//    say. This replay proves nothing about that.
//
// No credentials and no network. The two series below are a sample: the daily closes of 069500 and the
// daily opens and closes of 114800 on the same forty trading days, 2026-08-11 to 2026-10-12, as a
// public chart service reported them, unadjusted. They are here to make the example run, not as data
// to rely on, and the replay stamps them one calendar day apart from an arbitrary start, not with
// their real dates: replace them with your own history. A replay of this strategy must use the
// fund's real bars: negating the index's returns to invent the fund's would hide exactly the
// difference this file is warning about.
//
// Run it as:
//
//   example-inverse-fund

#include <algotradelib/backtest>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace strategy = algotradelib::strategy;
namespace backtest = algotradelib::backtest;

using algotradelib::Bar;
using algotradelib::BarInterval;
using algotradelib::BarStamp;
using algotradelib::Country;
using algotradelib::Currency;
using algotradelib::Decimal64;
using algotradelib::Instrument;
using algotradelib::InstrumentId;
using algotradelib::OrderSide;
using algotradelib::PriceRule;
using algotradelib::SecurityType;
using algotradelib::Venue;

/** @brief The instrument the signal is read from: a fund tracking the index. Never traded here. */
InstrumentId index_fund() { return InstrumentId{Country::KR, Venue::COMBINED, "069500"}; }

/** @brief The instrument that is traded: a fund targeting the opposite of the index's daily return. */
InstrumentId inverse_fund() { return InstrumentId{Country::KR, Venue::COMBINED, "114800"}; }

/**
 * @brief Holds a fixed number of inverse-fund shares while the index fund closes below its own recent
 *   average, and none otherwise.
 *
 * Long or flat, in one instrument. The quantity is in the fund's shares and is bounded by the caller:
 * it is not derived from a position in the index, and equal share counts would not be equal exposure.
 */
struct InverseFundOnDowntrend {
  /** @brief Where the signal is read. */
  InstrumentId signal_;
  /** @brief What is bought and sold. */
  InstrumentId fund_;
  /** @brief How many closes of the signal instrument the average is taken over. */
  std::size_t lookback_{5};
  /** @brief How many fund shares to hold while the signal is bearish. */
  Decimal64 quantity_{Decimal64::parse("1000")};

  std::vector<Decimal64> closes_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    // The fund's own bars are fed too, because its fills and its marks come from them. They carry
    // no signal.
    if (!(bar.instrument_ == signal_)) {
      return;
    }
    closes_.push_back(bar.bar_.close_);
    if (closes_.size() < lookback_) {
      return;
    }
    // `close * n < sum` instead of `close < sum / n`: exact, and no scale to choose.
    std::optional<Decimal64> sum = Decimal64::parse("0");
    for (std::size_t at = closes_.size() - lookback_; sum && at < closes_.size(); ++at) {
      sum = algotradelib::decimal_add(*sum, closes_[at]);
    }
    const auto scaled =
      algotradelib::decimal_multiply(closes_.back(), Decimal64::parse(std::to_string(lookback_)));
    if (!sum || !scaled) {
      return;
    }
    const bool bearish = algotradelib::decimal_less(*scaled, *sum);

    // A target for the fund, by the fund's name: never a negative target for the index.
    strategy::TargetPosition target;
    target.instrument_ = fund_;
    target.target_quantity_ = bearish ? quantity_ : Decimal64::parse("0");
    target.tag_ = "inverse-fund";
    ctx.submit(target);
  }
};

/** @brief Reference data for one of the two funds, as a live session would have fetched it. */
Instrument reference(const InstrumentId& id, const char* name) {
  Instrument instrument;
  instrument.id_ = id;
  instrument.name_ = name;
  instrument.type_ = SecurityType::ETF;
  instrument.currency_ = Currency{"KRW"};
  instrument.lot_size_ = Decimal64::parse("1");
  // This example sends market orders and states no price. A fund has its own tick grid, which is
  // not the equity ladder: take it from reference data before pricing an order for one.
  instrument.price_rule_ = PriceRule::unknown("an exchange-traded fund's tick grid is not stated here");
  return instrument;
}

/** @brief One day of the sample: the index fund's close, and the inverse fund's open and close. */
struct Day {
  int index_close_;
  int fund_open_;
  int fund_close_;
};

/** @brief Forty consecutive trading days (2026-08-11 to 2026-10-12) of the two funds. A sample. */
const std::vector<Day>& sample() {
  static const std::vector<Day> days = {
    {99265, 1147, 1126},  {103250, 1113, 1080}, {107270, 1043, 1039}, {110060, 1008, 1011},
    {108550, 984, 1026},  {101760, 1079, 1089}, {108190, 1054, 1021}, {109980, 1035, 1003},
    {105995, 1009, 1040}, {106795, 1070, 1032}, {107710, 1036, 1024}, {109135, 991, 1009},
    {107180, 1021, 1028}, {107615, 1057, 1025}, {107885, 1034, 1021}, {103500, 1054, 1063},
    {103610, 1047, 1061}, {105720, 1047, 1040}, {110820, 1004, 989},  {110335, 982, 994},
    {112100, 992, 978},   {111865, 982, 980},   {109500, 1018, 1002}, {105410, 1038, 1038},
    {104275, 1047, 1050}, {106365, 1050, 1029}, {106280, 1019, 1030}, {109285, 1003, 1000},
    {111700, 994, 978},   {111880, 957, 978},   {113145, 958, 966},   {109500, 971, 998},
    {109990, 1000, 993},  {109545, 986, 998},   {111520, 1006, 981},  {112060, 985, 975},
    {110745, 971, 987},   {108550, 999, 1008},  {105650, 1009, 1035}, {102695, 1046, 1065},
  };
  return days;
}

/** @brief A daily bar stamped at its close. */
Bar daily(std::chrono::system_clock::time_point at, int open, int close) {
  Bar bar;
  bar.timestamp_ = at;
  const auto opened = Decimal64::parse(std::to_string(open));
  const auto closed = Decimal64::parse(std::to_string(close));
  bar.open_ = opened;
  bar.close_ = closed;
  bar.high_ = open > close ? opened : closed;
  bar.low_ = open > close ? closed : opened;
  bar.volume_ = Decimal64::parse("1000000");
  return bar;
}

} // namespace

ATL_ASSERT_STRATEGY(InverseFundOnDowntrend, algotradelib::strategy::MinimalContext);
ATL_ASSERT_STRATEGY(InverseFundOnDowntrend, algotradelib::strategy::FullContext);

int main() {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("10000000");
  config.instruments_ = {
    reference(index_fund(), "an index fund (signal only)"),
    reference(inverse_fund(), "an inverse fund (traded)"),
  };
  config.series_capacity_ = 64;
  // Any phase, so the point stays the strategy: daily bars end outside the session.
  config.limits_.permitted_phases_ = std::nullopt;
  // The sellable check is left on, as it is by default. Every sell below is an exit of fund shares
  // the replay holds, so nothing here needs it off; a strategy that wanted a true short would.

  InverseFundOnDowntrend strategy_instance;
  strategy_instance.signal_ = index_fund();
  strategy_instance.fund_ = inverse_fund();

  backtest::Backtester<InverseFundOnDowntrend> engine(std::move(strategy_instance), config);
  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 20000);
  for (std::size_t at = 0; at < sample().size(); ++at) {
    const auto when = start + std::chrono::hours(24 * static_cast<int>(at));
    const auto& day = sample()[at];
    // The fund's bar first: an order decided on yesterday's signal fills at this open. Then the
    // signal's bar, on whose close the next decision is made.
    engine.feed(strategy::make_bar_event(
      inverse_fund(), BarInterval::ONE_DAY, BarStamp::CLOSE, daily(when, day.fund_open_, day.fund_close_)));
    engine.feed(strategy::make_bar_event(
      index_fund(), BarInterval::ONE_DAY, BarStamp::CLOSE, daily(when, day.index_close_, day.index_close_)));
  }
  engine.finish();

  const auto& result = engine.result();
  std::cout << "inverse fund 114800 traded on a signal from index fund 069500, " << sample().size()
            << " sample days -- no credentials, no network\n\n";
  for (const auto& execution : result.executions_) {
    const auto& fill = execution.fill_;
    const auto day = std::chrono::duration_cast<std::chrono::hours>(fill.at_ - start).count() / 24;
    std::cout << "  day " << day << "  " << (fill.side_ == OrderSide::BUY ? "BUY " : "SELL") << "  "
              << fill.quantity_.str() << " fund shares at " << fill.price_.str() << "\n";
  }
  std::cout << "\n  orders filled: " << result.executions_.size() << ", refused by the guard: "
            << result.refused_.size() << ", not buildable: " << result.rejected_.size() << "\n";
  if (!result.equity_.empty()) {
    std::cout << "  equity: " << config.cash_.str() << " -> " << result.equity_.back().equity_.str() << "\n";
  }
  std::cout
    << "\nEvery order above is for the fund, and every sell is an exit of shares bought earlier: the\n"
    << "account is never short, and the default sellable check never has anything to refuse.\n"
    << "The fund follows the opposite of the index's move one day at a time. Held across many days its\n"
    << "value is not what a short opened at the start would be worth, and it says nothing about any\n"
    << "single stock. Forty sample days rank nothing.\n";
  return 0;
}
