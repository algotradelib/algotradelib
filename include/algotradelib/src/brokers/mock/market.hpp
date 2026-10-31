#pragma once

#include "../../backtest/fill.hpp"
#include "../../backtest/ledger.hpp"
#include "../../strategy/event.hpp"

#include "../../core/market_data.hpp"
#include "../../core/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace mock {

/**
 * @brief The mock broker's tag: its order references are typed by it, so they work with no other
 *   broker.
 */
struct Broker {
  /** @brief The broker's name, `"mock"`. */
  static constexpr std::string_view name = "mock";
};

/**
 * @brief The environment the mock reports: always `"paper"`, never `"demo"` or `"real"`.
 *
 * The library has no environment type (KIS and Kiwoom configure demo or real; Toss has one
 * environment), so this is the statement a program maps when it reports where it trades.
 */
inline constexpr std::string_view environment = "paper";

/** @brief How a `Market` starts and what it charges. */
struct MarketSettings {
  /** @brief The starting cash, in the one currency the market keeps cash in. */
  Money cash_{Decimal64(0, 0), Currency{"KRW"}};
  /** @brief The market's first instant; bars that ended by then are history. */
  std::chrono::system_clock::time_point start_{};
  /** @brief The fill rule and its costs, as a backtest's; no commission, slippage or tax by default. */
  backtest::NextBarFillModel fills_{};
  /**
   * @brief The session window local dates and the close are read in, as a backtest's
   *   (`backtest::ReplayConfig::session_`): a `DAY` order lives for one session.
   */
  strategy::SessionWindow session_{strategy::SessionWindow::krx()};
};

/**
 * @brief A market in memory: bars the caller gives, a clock the caller moves, one account's cash,
 *   holdings and orders.
 *
 * A bar is applied when the clock passes its period end. Orders fill by the backtest's rule
 * (`backtest::NextBarFillModel`): an order trades against the first bar of its instrument whose
 * period began at or after the instant the market accepted it, a market order at that bar's open, a
 * limit order if the bar's range reaches the limit, never partly; the fill is stamped at the bar's
 * period end. A `DAY` order lives in the session of that first bar, and expires as a backtest's does:
 * after a daily bar of its session, after an intraday bar ending at or after the session's close, or
 * before a bar of a later date (a session whose closing bar is missing still ended). An
 * immediate-or-cancel or fill-or-kill order expires after its first bar. Expired orders are
 * `CANCELED`.
 *
 * Unlike a backtest, the market never fills an order against a bar that began before the order was
 * accepted, even where a backtest cannot tell and allows it (`ReplayResult::unchecked_fill_causality_`).
 *
 * Not modelled: trading hours (an order is accepted at any instant), a calendar, price bands, partial
 * fills, volume, settlement, short selling, interest. Cash is kept in one currency, and may go below
 * zero when a market buy fills above the price it reserved.
 *
 * If a fill cannot be costed (the fill model's commission has no rate for its date) or recorded (its
 * amount does not fit an exact decimal), or a position cannot be valued at a bar's close, the market
 * halts: `advance_to` throws `std::runtime_error` and keeps throwing, placements are refused
 * (`market_halted`), and the clock stays at the period end of the bar it halted on. What was done before
 * the failure stands, including fills of earlier orders on that bar; the order that failed stays
 * `PENDING`. Reads answer that state.
 *
 * Not thread-safe; nothing is persisted.
 */
class Market {
public:
  /**
   * @brief Builds a market at `settings.start_`.
   * @param bars One series per instrument, each strictly ascending, of a minute to daily interval;
   *   series may differ in interval, stamp and currency.
   * @param settings The starting cash, the first instant, the fill rule and the session window.
   * @param instruments Reference data for some or all of the instruments, as `instrument` answers it
   *   and as a backtest is given it (`backtest::ReplayConfig::instruments_`). An instrument without
   *   one gets an active stock in its series' currency, a lot of one share and no price grid
   *   (`PriceRule::unconstrained`).
   * @throws std::invalid_argument for two series of one instrument, a series not strictly ascending,
   *   a weekly, monthly or yearly series, a negative starting cash, or reference data for an
   *   instrument without bars.
   */
  Market(std::vector<BarSeries> bars, MarketSettings settings, std::vector<Instrument> instruments = {})
  : settings_(std::move(settings)),
    now_(settings_.start_),
    ledger_(settings_.cash_.amount_, settings_.cash_.currency_, backtest::ExecutionHistory::NONE) {
    if (decimal_less(settings_.cash_.amount_, Decimal64(0, 0))) {
      throw std::invalid_argument("the mock market's starting cash is negative");
    }
    listings_.reserve(bars.size());
    for (auto& series : bars) {
      if (!bar_interval_length(series.interval_) && series.interval_ != BarInterval::ONE_DAY) {
        throw std::invalid_argument("the mock market takes minute to daily bars only");
      }
      if (!series.ascending()) {
        throw std::invalid_argument("a mock market series is not strictly ascending");
      }
      if (find_listing(series.instrument_) != nullptr) {
        throw std::invalid_argument("the mock market was given two series of one instrument");
      }
      Listing listing;
      listing.reference_.id_ = series.instrument_;
      listing.reference_.type_ = SecurityType::STOCK;
      listing.reference_.status_ = InstrumentStatus::ACTIVE;
      listing.reference_.currency_ = series.currency_;
      listing.reference_.lot_size_ = Decimal64(1, 0);
      listing.reference_.price_rule_ = PriceRule::unconstrained();
      listing.series_ = std::move(series);
      while (listing.next_ < listing.series_.bars_.size() && !(now_ < period_end(listing, listing.next_))) {
        ++listing.next_;
      }
      listings_.push_back(std::move(listing));
    }
    for (auto& reference : instruments) {
      auto found = std::find_if(listings_.begin(), listings_.end(), [&](const Listing& listing) {
        return listing.series_.instrument_ == reference.id_;
      });
      if (found == listings_.end()) {
        throw std::invalid_argument("the mock market was given reference data for an instrument without bars");
      }
      found->reference_ = std::move(reference);
    }
  }

  /**
   * @brief Returns the market's clock.
   * @return The instant the market has reached.
   */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /**
   * @brief Returns why the market halted, if it did.
   * @return The reason, or `std::nullopt` while it trades.
   */
  const std::optional<std::string>& halted() const noexcept { return halted_; }

  /**
   * @brief Moves the clock to an instant, applying every bar whose period end it reaches, in order of
   *   period end across instruments (ties in the order the series were given).
   * @param to The instant; the clock never moves backwards.
   * @throws std::invalid_argument if `to` is before `now()`.
   * @throws std::runtime_error if the market halts on this call (the clock then stays at the period end
   *   of the bar it halted on; see the class) or had halted.
   */
  void advance_to(std::chrono::system_clock::time_point to) {
    if (to < now_) {
      throw std::invalid_argument("the mock market's clock does not move backwards");
    }
    if (halted_) {
      throw std::runtime_error(*halted_);
    }
    for (;;) {
      Listing* earliest = nullptr;
      for (auto& listing : listings_) {
        if (listing.next_ >= listing.series_.bars_.size()) {
          continue;
        }
        if (earliest == nullptr ||
            period_end(listing, listing.next_) < period_end(*earliest, earliest->next_)) {
          earliest = &listing;
        }
      }
      if (earliest == nullptr || to < period_end(*earliest, earliest->next_)) {
        break;
      }
      now_ = period_end(*earliest, earliest->next_);
      apply_bar(*earliest);
      if (halted_) {
        throw std::runtime_error(*halted_);
      }
      ++earliest->next_;
    }
    now_ = to;
  }

  /**
   * @brief Answers a bar request: bars whose period has ended by `now()`, never a forming one.
   * @param query The instrument, the interval (the series' own), the count and the newest timestamp.
   * @return The last `count_` such bars at or before `before_`, oldest first; `INVALID_ARGUMENT` with
   *   `unknown_instrument`, `unsupported_interval` or `bars_count` (a count below one).
   */
  Result<BarSeries> bars(const BarQuery& query) const {
    const auto* listing = find_listing(query.instrument_);
    if (listing == nullptr) {
      return refused<BarSeries>("unknown_instrument", "the mock market has no bars of this instrument");
    }
    if (query.interval_ != listing->series_.interval_) {
      return refused<BarSeries>("unsupported_interval", "the mock market has no bars of this interval");
    }
    if (query.count_ < 1) {
      return refused<BarSeries>("bars_count", "a bar request asks for at least one bar");
    }
    BarSeries answer;
    answer.instrument_ = listing->series_.instrument_;
    answer.interval_ = listing->series_.interval_;
    answer.stamp_ = listing->series_.stamp_;
    answer.currency_ = listing->series_.currency_;
    answer.adjusted_ = listing->series_.adjusted_;
    answer.received_at_ = now_;
    const auto& all = listing->series_.bars_;
    std::size_t end = 0;
    while (end < all.size() && !(now_ < period_end(*listing, end)) &&
           (!query.before_ || !(*query.before_ < all[end].timestamp_))) {
      ++end;
    }
    const auto count = static_cast<std::size_t>(query.count_);
    const std::size_t begin = end > count ? end - count : 0;
    answer.bars_.assign(all.begin() + static_cast<std::ptrdiff_t>(begin), all.begin() + static_cast<std::ptrdiff_t>(end));
    return Result<BarSeries>(std::move(answer));
  }

  /**
   * @brief Answers a quote: the close of the instrument's last bar that has ended by `now()`.
   * @param instrument The instrument.
   * @return The quote, `exchange_time_` the bar's period end; `INVALID_ARGUMENT` `unknown_instrument`,
   *   or `PROTOCOL` `no_price` before any bar of it has ended.
   */
  Result<QuoteSnapshot> quote(const InstrumentId& instrument) const {
    const auto* listing = find_listing(instrument);
    if (listing == nullptr) {
      return refused<QuoteSnapshot>("unknown_instrument", "the mock market has no bars of this instrument");
    }
    const auto last = last_closed(*listing);
    if (!last) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "no_price";
      error.message_ = "no bar of this instrument has ended yet";
      return Result<QuoteSnapshot>(std::move(error));
    }
    QuoteSnapshot quote;
    quote.instrument_ = listing->series_.instrument_;
    quote.last_price_ = listing->series_.bars_[*last].close_;
    quote.currency_ = listing->series_.currency_;
    quote.exchange_time_ = period_end(*listing, *last);
    quote.received_at_ = now_;
    return Result<QuoteSnapshot>(std::move(quote));
  }

  /**
   * @brief Answers a reference-data request.
   * @param instrument The instrument.
   * @return Its reference data, as given or defaulted at construction; `INVALID_ARGUMENT`
   *   `unknown_instrument`.
   */
  Result<Instrument> instrument(const InstrumentId& instrument) const {
    const auto* listing = find_listing(instrument);
    if (listing == nullptr) {
      return refused<Instrument>("unknown_instrument", "the mock market has no bars of this instrument");
    }
    return Result<Instrument>(listing->reference_);
  }

  /**
   * @brief Answers a holdings request.
   * @return Every position not flat, valued at its last applied bar's close, with per-currency totals.
   *   Every held share is sellable (basis: whole holding, resting sells to be deducted by the caller,
   *   unverified). `PROTOCOL` `out_of_range` if a row's cost or gain, or a total, does not fit an exact
   *   decimal.
   */
  Result<HoldingsSnapshot> holdings() const {
    HoldingsSnapshot snapshot;
    snapshot.received_at_ = now_;
    snapshot.sellable_basis_ = SellableBasis{
      SellableBasis::Coverage::WHOLE_HOLDING,
      SellableBasis::RestingTreatment::DEDUCT,
      false,
    };
    for (const auto& lot : ledger_.positions()) {
      if (lot.quantity_.coefficient() == 0) {
        continue;
      }
      auto row = lot;
      row.sellable_quantity_ = row.quantity_;
      const auto currency = lot.market_value_.currency_;
      const auto cost = decimal_multiply(row.quantity_, row.average_purchase_price_);
      const auto gain = cost ? decimal_subtract(row.market_value_.amount_, *cost) : std::nullopt;
      if (!gain || !add_to_totals(snapshot.totals_, row.market_value_.amount_, *cost, *gain, currency)) {
        BrokerError error;
        error.category_ = ErrorCategory::PROTOCOL;
        error.broker_code_ = "out_of_range";
        error.message_ = "a holding's value or the holdings' total does not fit an exact decimal";
        return Result<HoldingsSnapshot>(std::move(error));
      }
      row.purchase_value_ = Money{*cost, currency};
      row.unrealized_profit_loss_ = Money{*gain, currency};
      snapshot.positions_.push_back(std::move(row));
    }
    return Result<HoldingsSnapshot>(std::move(snapshot));
  }

  /**
   * @brief Answers a buying-power request: cash less what open buys reserve.
   *
   * An open limit buy reserves its quantity at its limit, an open market buy its quantity at the
   * last close when it was placed. The figure can be negative after a market buy filled above that.
   *
   * @param currency The currency.
   * @return The figure; `INVALID_ARGUMENT` `unknown_currency` for a currency the market keeps no cash
   *   in, `PROTOCOL` `out_of_range` if the arithmetic does not fit.
   */
  Result<BuyingPower> buying_power(const Currency& currency) const {
    const auto cash = ledger_.cash_in(currency);
    if (!cash) {
      return refused<BuyingPower>("unknown_currency", "the mock market keeps no cash in this currency");
    }
    const auto spendable = spendable_in(currency, *cash);
    if (!spendable) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "out_of_range";
      error.message_ = "the buying power does not fit an exact decimal";
      return Result<BuyingPower>(std::move(error));
    }
    BuyingPower power;
    power.cash_ = Money{*spendable, currency};
    power.received_at_ = now_;
    return Result<BuyingPower>(std::move(power));
  }

  /**
   * @brief Places an order.
   *
   * Checked in this order, each refusal `INVALID_ARGUMENT` with `reached_broker_` set:
   * `market_halted`; `unknown_instrument`; `unsupported_order_type` (market and limit only);
   * `unsupported_time_in_force` (`DAY`, immediate-or-cancel, fill-or-kill); `invalid_quantity` (not a
   * positive whole number); `invalid_price` (a limit without a positive price, or a market order
   * with one); `no_price` (a market buy before any bar of its instrument ended); `insufficient_cash`
   * (a buy reserving more than the buying power, or in a currency without cash);
   * `insufficient_holdings` (a sell of more than is held less open sells).
   *
   * @param order The order; its venue is not used.
   * @return The acknowledgement, the order id `"1"`, `"2"`, ... in the order accepted.
   */
  Result<PlacedOrder<Broker>> place(const OrderRequest& order) {
    if (halted_) {
      return refused<PlacedOrder<Broker>>("market_halted", "the mock market halted and takes no orders");
    }
    const auto* listing = find_listing(order.instrument_);
    if (listing == nullptr) {
      return refused<PlacedOrder<Broker>>("unknown_instrument", "the mock market has no bars of this instrument");
    }
    if (order.type_ != OrderType::MARKET && order.type_ != OrderType::LIMIT) {
      return refused<PlacedOrder<Broker>>("unsupported_order_type", "the mock market takes market and limit orders only");
    }
    switch (order.time_in_force_) {
      case TimeInForce::DAY:
      case TimeInForce::IMMEDIATE_OR_CANCEL:
      case TimeInForce::FILL_OR_KILL:
        break;
      case TimeInForce::UNKNOWN:
        return refused<PlacedOrder<Broker>>("unsupported_time_in_force", "the mock market needs a time in force");
    }
    const auto quantity = decimal_normalized(order.quantity_);
    if (quantity.coefficient() <= 0 || quantity.scale() != 0) {
      return refused<PlacedOrder<Broker>>("invalid_quantity", "an order's quantity is a positive whole number");
    }
    const bool limit = order.type_ == OrderType::LIMIT;
    if (limit != order.price_.has_value() ||
        (order.price_ && order.price_->coefficient() <= 0)) {
      return refused<PlacedOrder<Broker>>("invalid_price", "a limit order has a positive price and a market order none");
    }
    const auto currency = listing->series_.currency_;
    std::optional<Decimal64> reserve_price = order.price_;
    if (!reserve_price) {
      if (const auto last = last_closed(*listing)) {
        reserve_price = listing->series_.bars_[*last].close_;
      }
    }
    if (order.side_ == OrderSide::BUY) {
      if (!reserve_price) {
        return refused<PlacedOrder<Broker>>("no_price", "no bar of this instrument has ended to price a market buy at");
      }
      const auto cash = ledger_.cash_in(currency);
      const auto spendable = cash ? spendable_in(currency, *cash) : std::nullopt;
      const auto cost = decimal_multiply(quantity, *reserve_price);
      if (!spendable || !cost || decimal_less(*spendable, *cost)) {
        return refused<PlacedOrder<Broker>>("insufficient_cash", "the buy costs more than the buying power");
      }
    } else {
      const auto held = ledger_.quantity_of(order.instrument_).value_or(Decimal64(0, 0));
      const auto free = decimal_subtract(held, open_sells_of(order.instrument_));
      if (!free || decimal_less(*free, quantity)) {
        return refused<PlacedOrder<Broker>>("insufficient_holdings", "the sell is more than the shares held and not already offered");
      }
    }
    MarketOrder accepted;
    accepted.snapshot_.order_id_ = std::to_string(++last_id_);
    accepted.snapshot_.instrument_ = order.instrument_;
    accepted.snapshot_.side_ = order.side_;
    accepted.snapshot_.type_ = order.type_;
    accepted.snapshot_.time_in_force_ = order.time_in_force_;
    accepted.snapshot_.session_ = order.session_;
    accepted.snapshot_.venue_ = order.venue_;
    accepted.snapshot_.status_ = OrderStatus::PENDING;
    accepted.snapshot_.price_ = order.price_;
    accepted.snapshot_.quantity_ = quantity;
    accepted.snapshot_.currency_ = currency;
    accepted.snapshot_.ordered_at_ = now_;
    accepted.snapshot_.execution_.filled_quantity_ = Decimal64(0, 0);
    accepted.reserve_price_ = reserve_price;
    orders_.push_back(std::move(accepted));
    PlacedOrder<Broker> placed;
    placed.ack_.order_id_ = orders_.back().snapshot_.order_id_;
    placed.ack_.accepted_at_ = now_;
    placed.ref_.primary_ = placed.ack_.order_id_;
    return Result<PlacedOrder<Broker>>(std::move(placed));
  }

  /**
   * @brief Cancels the whole remainder of an open order.
   * @param order_id The order id.
   * @return The acknowledgement; `INVALID_ARGUMENT` `nothing_to_cancel` (with `reached_broker_` set)
   *   for an id that is unknown or already finished.
   */
  Result<PlacedOrder<Broker>> cancel(const std::string& order_id) {
    for (auto& order : orders_) {
      if (order.snapshot_.order_id_ != order_id) {
        continue;
      }
      if (order.snapshot_.status_ != OrderStatus::PENDING) {
        break;
      }
      order.snapshot_.status_ = OrderStatus::CANCELED;
      order.snapshot_.canceled_at_ = now_;
      PlacedOrder<Broker> placed;
      placed.ack_.order_id_ = order_id;
      placed.ack_.accepted_at_ = now_;
      placed.ref_.primary_ = order_id;
      return Result<PlacedOrder<Broker>>(std::move(placed));
    }
    return refused<PlacedOrder<Broker>>("nothing_to_cancel", "no open order has this id");
  }

  /**
   * @brief Answers a working-order request.
   * @return The open orders, oldest first, each with its whole quantity amendable. A finished order
   *   leaves the list; read its outcome from `order_history`.
   */
  WorkingOrders<Broker> working_orders() const {
    WorkingOrders<Broker> listing;
    listing.received_at_ = now_;
    for (const auto& order : orders_) {
      if (order.snapshot_.status_ != OrderStatus::PENDING) {
        continue;
      }
      WorkingOrder<Broker> row;
      row.order_ = order.snapshot_;
      row.ref_.primary_ = order.snapshot_.order_id_;
      row.amendable_quantity_ = order.snapshot_.quantity_;
      listing.orders_.push_back(std::move(row));
    }
    return listing;
  }

  /**
   * @brief Answers an order-history request: every order, oldest first, in one page.
   * @param query `from_` and `to_` keep orders accepted on those local dates (inclusive); the cursor,
   *   symbol and limit are not read.
   * @return The page, each order with its status (`PENDING`, `FILLED` or `CANCELED`) and execution.
   */
  OrderPage order_history(const OrderHistoryQuery& query) const {
    OrderPage page;
    page.received_at_ = now_;
    for (const auto& order : orders_) {
      const auto date = settings_.session_.local_date(*order.snapshot_.ordered_at_);
      if ((query.from_ && date < *query.from_) || (query.to_ && *query.to_ < date)) {
        continue;
      }
      page.orders_.push_back(order.snapshot_);
    }
    return page;
  }

private:
  /** One instrument's bars and how far the clock has applied them. */
  struct Listing {
    /** The bars. */
    BarSeries series_;
    /** The reference data. */
    Instrument reference_;
    /** The first bar not yet applied. */
    std::size_t next_{0};
  };

  /** One order the market accepted. */
  struct MarketOrder {
    /** The order as the history reports it. */
    OrderSnapshot snapshot_;
    /** The price a buy reserves at: its limit, or the last close when a market order was placed. */
    std::optional<Decimal64> reserve_price_;
    /** The local date of the first bar it was eligible against, which a `DAY` order lives in. */
    std::optional<Date> session_;
  };

  /** Returns a refusal the market answered. */
  template <typename T>
  static Result<T> refused(const char* code, const char* message) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = true;
    error.broker_code_ = code;
    error.message_ = message;
    return Result<T>(std::move(error));
  }

  /** Returns the listing of an instrument, or `nullptr`. */
  const Listing* find_listing(const InstrumentId& instrument) const {
    for (const auto& listing : listings_) {
      if (listing.series_.instrument_ == instrument) {
        return &listing;
      }
    }
    return nullptr;
  }

  /** Returns when a bar's period ended. */
  static std::chrono::system_clock::time_point period_end(const Listing& listing, std::size_t at) {
    return strategy::bar_period_end(listing.series_.bars_[at], listing.series_.interval_, listing.series_.stamp_);
  }

  /** Returns when a bar's period began, as the backtest derives it. */
  std::chrono::system_clock::time_point period_start(const Listing& listing, std::size_t at) const {
    const auto& series = listing.series_;
    if (series.stamp_ == BarStamp::OPEN) {
      return series.bars_[at].timestamp_;
    }
    if (const auto length = bar_interval_length(series.interval_)) {
      return period_end(listing, at) - *length;
    }
    return settings_.session_.opens_at(period_end(listing, at));
  }

  /** Returns the last bar that has ended by `now_`, if any. */
  std::optional<std::size_t> last_closed(const Listing& listing) const {
    std::optional<std::size_t> last;
    for (std::size_t at = 0; at < listing.series_.bars_.size() && !(now_ < period_end(listing, at)); ++at) {
      last = at;
    }
    return last;
  }

  /** Returns the quantity open sells of an instrument offer. */
  Decimal64 open_sells_of(const InstrumentId& instrument) const {
    Decimal64 total(0, 0);
    for (const auto& order : orders_) {
      if (order.snapshot_.status_ == OrderStatus::PENDING && order.snapshot_.side_ == OrderSide::SELL &&
          order.snapshot_.instrument_ == instrument) {
        total = decimal_add(total, order.snapshot_.quantity_).value_or(total);
      }
    }
    return total;
  }

  /** Returns cash less what open buys in a currency reserve, if it fits. */
  std::optional<Decimal64> spendable_in(const Currency& currency, const Decimal64& cash) const {
    std::optional<Decimal64> left = cash;
    for (const auto& order : orders_) {
      if (!left) {
        break;
      }
      if (order.snapshot_.status_ != OrderStatus::PENDING || order.snapshot_.side_ != OrderSide::BUY ||
          !(order.snapshot_.currency_ == currency) || !order.reserve_price_) {
        continue;
      }
      const auto reserved = decimal_multiply(order.snapshot_.quantity_, *order.reserve_price_);
      left = reserved ? decimal_subtract(*left, *reserved) : std::nullopt;
    }
    return left;
  }

  /** Adds a holding's figures to its currency's totals; `false`, changing nothing, if a sum does not fit. */
  static bool add_to_totals(
    std::vector<HoldingsTotals>& totals,
    const Decimal64& market_value,
    const Decimal64& cost,
    const Decimal64& gain,
    const Currency& currency
  ) {
    auto found = std::find_if(totals.begin(), totals.end(), [&](const HoldingsTotals& t) { return t.currency_ == currency; });
    const auto zero = Decimal64(0, 0);
    const auto purchase = decimal_add(found == totals.end() ? zero : found->purchase_value_, cost);
    const auto market = decimal_add(found == totals.end() ? zero : found->market_value_, market_value);
    const auto unrealized = decimal_add(found == totals.end() ? zero : found->unrealized_profit_loss_, gain);
    if (!purchase || !market || !unrealized) {
      return false;
    }
    if (found == totals.end()) {
      HoldingsTotals fresh;
      fresh.currency_ = currency;
      totals.push_back(fresh);
      found = totals.end() - 1;
    }
    found->purchase_value_ = *purchase;
    found->market_value_ = *market;
    found->unrealized_profit_loss_ = *unrealized;
    return true;
  }

  /** Ends an order unfilled at an instant. */
  static void cancel_at(MarketOrder& order, std::chrono::system_clock::time_point at) {
    order.snapshot_.status_ = OrderStatus::CANCELED;
    order.snapshot_.canceled_at_ = at;
  }

  /** Expires a listing's `DAY` orders whose session ended: of an earlier date, or of `date` when `ends`. */
  void expire_day_orders(const Listing& listing, const Date& date, bool ends, std::chrono::system_clock::time_point at) {
    for (auto& order : orders_) {
      if (order.snapshot_.status_ != OrderStatus::PENDING || !order.session_ ||
          order.snapshot_.time_in_force_ != TimeInForce::DAY ||
          !(order.snapshot_.instrument_ == listing.series_.instrument_)) {
        continue;
      }
      if (*order.session_ < date || (ends && *order.session_ == date)) {
        cancel_at(order, at);
      }
    }
  }

  /** Expires, fills and marks against a listing's next bar, as a backtest settles a bar; may halt. */
  void apply_bar(Listing& listing) {
    const auto at = listing.next_;
    const auto& series = listing.series_;
    const auto& session = settings_.session_;
    strategy::BarEvent event;
    event.instrument_ = series.instrument_;
    event.interval_ = series.interval_;
    event.stamp_ = series.stamp_;
    event.bar_ = series.bars_[at];
    event.period_end_ = period_end(listing, at);
    const auto start = period_start(listing, at);
    const auto date = session.local_date(event.bar_.timestamp_);
    const auto fill_date = session.local_date(event.period_end_);
    const auto open_date = series.stamp_ == BarStamp::OPEN ? std::optional<Date>(date) : std::nullopt;
    const auto closes = session.opens_at(event.bar_.timestamp_) - session.open_ + session.close_;
    const bool ends_session = !bar_interval_length(series.interval_) || !(event.period_end_ < closes);
    expire_day_orders(listing, date, false, event.period_end_);
    for (auto& order : orders_) {
      auto& snapshot = order.snapshot_;
      if (snapshot.status_ != OrderStatus::PENDING || !(snapshot.instrument_ == series.instrument_) ||
          start < *snapshot.ordered_at_) {
        continue;
      }
      if (!order.session_) {
        order.session_ = date;
      }
      strategy::PlannedOrder planned;
      planned.instrument_ = snapshot.instrument_;
      planned.side_ = snapshot.side_;
      planned.type_ = snapshot.type_;
      planned.time_in_force_ = snapshot.time_in_force_;
      planned.quantity_ = snapshot.quantity_;
      planned.price_ = snapshot.price_;
      const auto decision = settings_.fills_.decide(planned, event, listing.reference_.price_rule_, fill_date, open_date);
      if (decision.costing_error_) {
        halted_ = "the mock market halted: order " + snapshot.order_id_ + "'s fill could not be costed (" +
          decision.costing_error_->message_ + ")";
        return;
      }
      if (decision.execution_) {
        auto execution = *decision.execution_;
        execution.fill_.order_id_ = snapshot.order_id_;
        execution.currency_ = snapshot.currency_;
        if (!ledger_.apply(execution, snapshot.currency_)) {
          halted_ = "the mock market halted: order " + snapshot.order_id_ + "'s fill does not fit an exact decimal";
          return;
        }
        record_fill(snapshot, execution, event.period_end_);
        continue;
      }
      if (!decision.still_working_) {
        cancel_at(order, event.period_end_);
      }
    }
    expire_day_orders(listing, date, ends_session, event.period_end_);
    if (!ledger_.mark(series.instrument_, event.bar_.close_)) {
      halted_ = "the mock market halted: a position's value at the close of " + series.instrument_.symbol_ +
        " does not fit an exact decimal";
    }
  }

  /** Records a whole fill on an order's snapshot. */
  static void record_fill(
    OrderSnapshot& snapshot,
    const backtest::Execution& execution,
    std::chrono::system_clock::time_point at
  ) {
    const auto& fill = execution.fill_;
    snapshot.status_ = OrderStatus::FILLED;
    auto& done = snapshot.execution_;
    done.filled_quantity_ = fill.quantity_;
    done.average_filled_price_ = fill.price_;
    if (const auto amount = decimal_multiply(fill.quantity_, fill.price_)) {
      done.filled_amount_ = Money{*amount, snapshot.currency_};
    }
    auto commission = execution.commission_;
    if (execution.institution_fee_) {
      commission = decimal_add(commission, *execution.institution_fee_).value_or(commission);
    }
    done.commission_ = Money{commission, snapshot.currency_};
    done.tax_ = Money{execution.tax_, snapshot.currency_};
    done.filled_at_ = at;
  }

  MarketSettings settings_;
  std::chrono::system_clock::time_point now_;
  backtest::ReplayLedger ledger_;
  std::vector<Listing> listings_;
  std::vector<MarketOrder> orders_;
  std::uint64_t last_id_{0};
  std::optional<std::string> halted_;
};

} // namespace mock
} // namespace algotradelib
