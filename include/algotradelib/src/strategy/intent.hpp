#pragma once

#include <algotradelib/core>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace algotradelib {
namespace strategy {

/**
 * @file
 * @brief What a strategy asks the runner for: orders (`OrderIntent`), target positions
 *   (`TargetPosition`) and cancellations (`CancelIntent`).
 *
 * These are plain values that name no broker; the runner turns them into broker orders. A limit
 * price is a `Price`, which can only be made on the instrument's tick grid, and only a limit or
 * stop-limit order can carry one.
 */

/**
 * @brief A price known to be on its instrument's tick grid.
 *
 * Make one with `checked` (for a price you have) or `rounded` (to move a price onto the grid).
 * For a price computed as a `Scalar`, use `analytics::round_scalar_to_tick` first.
 */
class Price {
public:
  /**
   * @brief Makes a price from a value that is already on the grid.
   *
   * @param value The price.
   * @param rule The instrument's tick rule (from its `Instrument` reference data).
   * @return The price, or `std::nullopt` if `value` is off the grid or `rule` does not know the
   *   grid.
   */
  static std::optional<Price> checked(Decimal64 value, const PriceRule& rule) {
    const auto aligned = rule.on_tick(value);
    if (!aligned || !*aligned) {
      return std::nullopt;
    }
    return Price(std::move(value));
  }

  /**
   * @brief Makes a price by rounding a value onto the grid, in the order's favour.
   *
   * A buy rounds down and a sell rounds up, so the price never becomes worse for the order.
   *
   * @param value The price to round.
   * @param rule The instrument's tick rule.
   * @param side The side of the order the price is for.
   * @return The rounded price, or `std::nullopt` if `rule` does not know the grid.
   */
  static std::optional<Price> rounded(
    const Decimal64& value,
    const PriceRule& rule,
    OrderSide side
  ) {
    auto moved = rule.round_to_tick(value, side);
    if (!moved) {
      return std::nullopt;
    }
    return Price(std::move(*moved));
  }

  /**
   * @brief Returns the price as a decimal.
   * @return The exact value the runner sends.
   */
  const Decimal64& value() const noexcept { return value_; }

  /**
   * @brief Returns the price as text, for logs.
   * @return The decimal's text.
   */
  std::string str() const { return value_.str(); }

  /**
   * @brief Compares two prices by exact value (scale included; see `Decimal64`).
   * @param lhs The first price.
   * @param rhs The second price.
   * @return `true` if they are equal.
   */
  friend bool operator==(const Price& lhs, const Price& rhs) {
    return lhs.value_ == rhs.value_;
  }

  /**
   * @brief Compares two prices for inequality.
   * @param lhs The first price.
   * @param rhs The second price.
   * @return `true` if they differ.
   */
  friend bool operator!=(const Price& lhs, const Price& rhs) { return !(lhs == rhs); }

private:
  explicit Price(Decimal64 value) : value_(std::move(value)) {}

  Decimal64 value_;
};

/** @brief How an order's size is expressed. */
enum class SizeKind {
  /** @brief A number of shares or contracts. */
  QUANTITY,
  /** @brief An amount of money; the runner divides it by a price. */
  NOTIONAL,
  /** @brief A fraction of portfolio value to hold; the runner trades the difference. */
  TARGET_WEIGHT,
};

/**
 * @brief How much an `OrderIntent` trades: a quantity, a notional amount, or a target weight.
 *
 * The runner converts a notional or a weight into a quantity, using a price and the portfolio.
 * Read the value with the accessor that matches `kind()`; the others throw.
 */
class SizeSpec {
public:
  /** @brief Constructs a quantity of zero, which the runner ignores. */
  SizeSpec() = default;

  /**
   * @brief Makes a size in shares or contracts.
   * @param amount The quantity.
   * @return The size.
   */
  static SizeSpec quantity(Decimal64 amount) {
    SizeSpec size;
    size.kind_ = SizeKind::QUANTITY;
    size.quantity_ = std::move(amount);
    return size;
  }

  /**
   * @brief Makes a size as an amount of money.
   * @param amount The amount; the runner divides it by a price to get a quantity.
   * @return The size.
   */
  static SizeSpec notional(Money amount) {
    SizeSpec size;
    size.kind_ = SizeKind::NOTIONAL;
    size.notional_ = std::move(amount);
    return size;
  }

  /**
   * @brief Makes a size as a fraction of portfolio value to hold.
   *
   * A weight states a holding, not a trade: the runner compares it with what is held and working
   * and trades only the difference, so restating the same weight places nothing. Like a target
   * position, it may withdraw working orders on the instrument (see `strategy::weight_target`).
   * The target is rounded down to whole lots, so a weight worth less than one share sells the
   * holding to zero; set a `TradeTolerance` to avoid trading one share each time the price ticks.
   *
   * Only a buy intent with `DAY` validity, no trigger and no idempotency key can carry a weight;
   * anything else is refused with `RejectReason::UNSUPPORTED_WEIGHT`. A weight needs the portfolio
   * value: a backtest supplies it, but a live runner refuses the intent (`no_portfolio_value`)
   * unless you call `Runner::set_portfolio_value`.
   *
   * It is still an order intent, not a `TargetPosition`: one withheld for the account is dropped,
   * not retained for later; one refused for the market phase is kept, as any intent is.
   *
   * @param fraction The fraction of portfolio value, as an exact decimal (for example `0.25`); for
   *   a computed weight, convert with `analytics::to_decimal`.
   * @return The size.
   */
  static SizeSpec target_weight(Decimal64 fraction) {
    SizeSpec size;
    size.kind_ = SizeKind::TARGET_WEIGHT;
    size.weight_ = std::move(fraction);
    return size;
  }

  /**
   * @brief Returns how the size is expressed.
   * @return The kind, which selects the accessor to read.
   */
  SizeKind kind() const noexcept { return kind_; }

  /**
   * @brief Returns the quantity.
   * @return The number of shares or contracts.
   * @throws std::logic_error unless `kind()` is `QUANTITY`.
   */
  const Decimal64& as_quantity() const {
    require(SizeKind::QUANTITY, "quantity");
    return quantity_;
  }

  /**
   * @brief Returns the notional amount.
   * @return The amount of money.
   * @throws std::logic_error unless `kind()` is `NOTIONAL`.
   */
  const Money& as_notional() const {
    require(SizeKind::NOTIONAL, "notional");
    return notional_;
  }

  /**
   * @brief Returns the target weight.
   * @return The fraction of portfolio value.
   * @throws std::logic_error unless `kind()` is `TARGET_WEIGHT`.
   */
  const Decimal64& as_target_weight() const {
    require(SizeKind::TARGET_WEIGHT, "target weight");
    return weight_;
  }

private:
  void require(SizeKind wanted, const char* name) const {
    if (kind_ != wanted) {
      throw std::logic_error(std::string("order size is not a ") + name);
    }
  }

  SizeKind kind_{SizeKind::QUANTITY};
  Decimal64 quantity_;
  Money notional_;
  Decimal64 weight_;
};

/**
 * @brief One order a strategy asks the runner to place.
 *
 * Make one with `limit`, `market` or `stop_limit`; add a tag with `with_tag` and an idempotency
 * key with `with_client_order_id`. Submit it with `ctx.submit(intent)`.
 */
class OrderIntent {
public:
  /**
   * @brief Makes a limit order.
   *
   * @param instrument The instrument to trade.
   * @param side Buy or sell.
   * @param size How much to trade; see `SizeSpec`.
   * @param limit The limit price.
   * @param time_in_force How long the order stays valid.
   * @return The intent.
   */
  static OrderIntent limit(
    InstrumentId instrument,
    OrderSide side,
    SizeSpec size,
    Price limit,
    TimeInForce time_in_force = TimeInForce::DAY
  ) {
    OrderIntent intent;
    intent.instrument_ = std::move(instrument);
    intent.side_ = side;
    intent.type_ = OrderType::LIMIT;
    intent.time_in_force_ = time_in_force;
    intent.size_ = std::move(size);
    intent.limit_ = std::move(limit);
    return intent;
  }

  /**
   * @brief Makes a stop-limit order: a limit order the broker releases when the trigger price is
   *   reached.
   *
   * Which direction of move reaches the trigger, and how the trigger may relate to the limit, is
   * the broker's rule and is not checked here. A backtest models the release and the fill with
   * stated assumptions and counts them in `ReportCaveats`; see `NextBarFillModel::decide`.
   *
   * @param instrument The instrument to trade.
   * @param side Buy or sell.
   * @param size How much to trade; see `SizeSpec`.
   * @param trigger The price that releases the order.
   * @param limit The limit price once released.
   * @param time_in_force How long the order stays valid.
   * @return The intent.
   */
  static OrderIntent stop_limit(
    InstrumentId instrument,
    OrderSide side,
    SizeSpec size,
    Price trigger,
    Price limit,
    TimeInForce time_in_force = TimeInForce::DAY
  ) {
    OrderIntent intent;
    intent.instrument_ = std::move(instrument);
    intent.side_ = side;
    intent.type_ = OrderType::STOP_LIMIT;
    intent.time_in_force_ = time_in_force;
    intent.size_ = std::move(size);
    intent.limit_ = std::move(limit);
    intent.trigger_ = std::move(trigger);
    return intent;
  }

  /**
   * @brief Makes a market order, which carries no price.
   *
   * @param instrument The instrument to trade.
   * @param side Buy or sell.
   * @param size How much to trade; see `SizeSpec`.
   * @param time_in_force How long the order stays valid.
   * @return The intent.
   */
  static OrderIntent market(
    InstrumentId instrument,
    OrderSide side,
    SizeSpec size,
    TimeInForce time_in_force = TimeInForce::DAY
  ) {
    OrderIntent intent;
    intent.instrument_ = std::move(instrument);
    intent.side_ = side;
    intent.type_ = OrderType::MARKET;
    intent.time_in_force_ = time_in_force;
    intent.size_ = std::move(size);
    return intent;
  }

  /**
   * @brief Returns a copy with a tag.
   * @param tag A label of your choice, carried to the order and its fills; the library does not
   *   interpret it.
   * @return The tagged copy.
   */
  OrderIntent with_tag(std::string tag) const {
    OrderIntent copy = *this;
    copy.tag_ = std::move(tag);
    return copy;
  }

  /**
   * @brief Returns a copy with a broker idempotency key.
   *
   * Set it only where `Ctx::caps::has_client_order_id` is `true` (test it with `if constexpr`):
   * a runner whose broker has no idempotency key refuses the intent rather than dropping the key.
   *
   * @param key The key.
   * @return The copy carrying `key`.
   */
  OrderIntent with_client_order_id(std::string key) const {
    OrderIntent copy = *this;
    copy.client_order_id_ = std::move(key);
    return copy;
  }

  /**
   * @brief Returns the instrument to trade.
   * @return The instrument.
   */
  const InstrumentId& instrument() const noexcept { return instrument_; }

  /**
   * @brief Returns the side.
   * @return Buy or sell.
   */
  OrderSide side() const noexcept { return side_; }

  /**
   * @brief Returns the order type.
   * @return `LIMIT`, `MARKET` or `STOP_LIMIT`.
   */
  OrderType type() const noexcept { return type_; }

  /**
   * @brief Returns how long the order stays valid.
   * @return The time in force.
   */
  TimeInForce time_in_force() const noexcept { return time_in_force_; }

  /**
   * @brief Returns how much to trade.
   * @return The size.
   */
  const SizeSpec& size() const noexcept { return size_; }

  /**
   * @brief Returns the limit price.
   * @return The price for a limit or stop-limit order; `std::nullopt` for a market order.
   */
  const std::optional<Price>& limit() const noexcept { return limit_; }

  /**
   * @brief Returns the trigger price.
   * @return The trigger for a stop-limit order; `std::nullopt` otherwise.
   */
  const std::optional<Price>& trigger() const noexcept { return trigger_; }

  /**
   * @brief Returns the tag.
   * @return The label set with `with_tag`, or an empty string.
   */
  const std::string& tag() const noexcept { return tag_; }

  /**
   * @brief Returns the idempotency key.
   * @return The key set with `with_client_order_id`, or `std::nullopt`.
   */
  const std::optional<std::string>& client_order_id() const noexcept {
    return client_order_id_;
  }

private:
  OrderIntent() = default;

  InstrumentId instrument_;
  OrderSide side_{OrderSide::BUY};
  OrderType type_{OrderType::LIMIT};
  TimeInForce time_in_force_{TimeInForce::DAY};
  SizeSpec size_;
  std::optional<Price> limit_;
  std::optional<Price> trigger_;
  std::string tag_;
  std::optional<std::string> client_order_id_;
};

/**
 * @brief The position a strategy wants to hold in one instrument.
 *
 * The runner compares the target with the current position **and the orders still working**,
 * and places only what is needed to reach it. Prefer it to `OrderIntent`: emitting "buy 100" again
 * after 40 filled and 60 are working would buy 100 more, while a target of 100 buys nothing more.
 * Submit it with `ctx.submit(target)`.
 */
struct TargetPosition {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The position to hold, signed: negative is short, and zero means flat. */
  Decimal64 target_quantity_;
  /** @brief The limit price for the orders the runner places; without one they are market orders. */
  std::optional<Price> limit_;
  /** @brief A label carried to the orders and their fills. */
  std::string tag_;
};

/**
 * @brief A request to cancel one working order.
 *
 * A strategy does not name the broker type: take the reference from the portfolio with `auto`
 * and pass it to `ctx.cancel(...)`:
 *
 * ```cpp
 * template <class Ctx>
 * void on_bar(Ctx& ctx, const BarEvent&) {
 *   for (const auto& working : ctx.portfolio().working_) {
 *     ctx.cancel(working.ref_);
 *   }
 * }
 * ```
 *
 * @tparam Broker The broker tag; a reference from one broker cannot be passed to another.
 */
template <typename Broker>
struct CancelIntent {
  /** @brief The order to cancel. */
  OrderRef<Broker> ref_;
  /** @brief A label for attribution. */
  std::string tag_;
};

} // namespace strategy
} // namespace algotradelib
