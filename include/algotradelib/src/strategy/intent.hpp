#pragma once

#include <algotradelib/core>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace algotradelib {
namespace strategy {

/**
 * What a strategy asks for, as values it can construct and nothing it can send.
 *
 * A strategy emits intents; only the runner turns them into `OrderRequest`s and performs I/O.
 * The types here therefore carry no broker, no session, and no transport — which is what makes
 * one strategy source compile against every broker.
 *
 * Two invariants are enforced by construction rather than by review, because both are the kind
 * of mistake that reaches an exchange before anyone notices:
 *
 * 1. **A limit price must sit on its instrument's tick grid.** `Price` has no public
 *    constructor; the only ways to get one validate against a `PriceRule`. An indicator value
 *    correct to four decimal places and off the ladder is rejected by an exchange exactly as
 *    firmly as a wrong one, and a library that produced it confidently would be worse than one
 *    that refused.
 * 2. **A limit order carries a price and a market order does not.** The factories make the
 *    other two combinations unconstructible, rather than leaving an adapter to reject them
 *    after the strategy believed it had placed an order.
 */

/**
 * A price proven to sit on its instrument's tick grid.
 *
 * There is no public constructor and no default. The only routes in are `checked`, which
 * verifies a price already in hand, and `rounded`, which moves one onto the grid first. That is
 * the whole point of the type: an `OrderIntent`'s limit can only be a `Price`, so an off-grid
 * value cannot reach the runner, and the failure is a `std::nullopt` at the point of
 * construction rather than a broker rejection later.
 *
 * Deliberately not constructible from `math::Scalar`. A strategy computing a price from an
 * indicator converts it through `analytics::round_scalar_to_tick` — the one sanctioned crossing
 * of the decimal/scalar boundary — and hands the exact result here. Adding a `Scalar` overload
 * would put a second crossing in a second place, and would make this header depend on the
 * analytics layer and therefore on a linear-algebra backend, which a strategy placing market
 * orders has no reason to need.
 */
class Price {
public:
  /**
   * Returns `value` as a price if it sits on the grid `rule` describes.
   *
   * Nothing when it does not, and nothing when `rule` does not know the grid — an unknown
   * ladder answers neither yes nor no, and answering yes would let an off-grid price through a
   * check that appeared to pass.
   */
  static std::optional<Price> checked(Decimal64 value, const PriceRule& rule) {
    const auto aligned = rule.on_tick(value);
    if (!aligned || !*aligned) {
      return std::nullopt;
    }
    return Price(std::move(value));
  }

  /**
   * Returns `value` moved onto the grid in the direction that cannot cross it.
   *
   * A buy rounds down and a sell rounds up, so rounding never improves a price into territory
   * the caller did not ask for. Nothing when the grid is unknown.
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

  /** Returns the exact decimal the runner will send. */
  const Decimal64& value() const noexcept { return value_; }

  /** Returns the price as text, for logs and attribution. */
  std::string str() const { return value_.str(); }

  /** Compares two prices by exact value. */
  friend bool operator==(const Price& lhs, const Price& rhs) {
    return lhs.value_ == rhs.value_;
  }

  /** Compares two prices for inequality. */
  friend bool operator!=(const Price& lhs, const Price& rhs) { return !(lhs == rhs); }

private:
  explicit Price(Decimal64 value) : value_(std::move(value)) {}

  Decimal64 value_;
};

/** Which way a strategy expressed the size of an order. */
enum class SizeKind {
  /** A number of shares or contracts, exactly as the order will carry it. */
  QUANTITY,
  /** An amount of money to spend or realize; the runner divides by a price. */
  NOTIONAL,
  /** A fraction of portfolio value to hold; the runner diffs against the position. */
  TARGET_WEIGHT,
};

/**
 * How much to trade, in whichever of three currencies of measurement suits the strategy.
 *
 * A signal strategy usually knows a quantity, a cash-allocating one knows a notional, and a
 * cross-sectional one knows a weight. Converting a notional or a weight needs a price and the
 * portfolio, which only the runner has, so the conversion happens there and the strategy stays
 * free of it.
 *
 * The accessors throw rather than returning a default for the wrong kind. A size read as the
 * wrong currency of measurement is an order for the wrong amount, which is not a failure to
 * paper over with a zero.
 */
class SizeSpec {
public:
  /** Constructs a size of zero quantity, which the runner drops as a no-op. */
  SizeSpec() = default;

  /** Returns a size of exactly `amount` shares or contracts. */
  static SizeSpec quantity(Decimal64 amount) {
    SizeSpec size;
    size.kind_ = SizeKind::QUANTITY;
    size.quantity_ = std::move(amount);
    return size;
  }

  /** Returns a size of `amount` of money, to be divided by a price by the runner. */
  static SizeSpec notional(Money amount) {
    SizeSpec size;
    size.kind_ = SizeKind::NOTIONAL;
    size.notional_ = std::move(amount);
    return size;
  }

  /**
   * Returns a size expressed as a fraction of portfolio value.
   *
   * An exact decimal, not a `Scalar`, and at whatever scale the caller chooses: a third of a
   * portfolio is `0.3333` or `0.333333` depending on how precisely they mean it, and making them
   * say leaves no doubt. A strategy computing weights from indicators — inverse volatility, say —
   * crosses the boundary once through `analytics::to_decimal` and passes the exact result, the
   * same pattern `Price` follows. That keeps this whole layer free of any decimal/scalar crossing
   * and therefore free of a linear-algebra backend.
   */
  static SizeSpec target_weight(Decimal64 fraction) {
    SizeSpec size;
    size.kind_ = SizeKind::TARGET_WEIGHT;
    size.weight_ = std::move(fraction);
    return size;
  }

  /** Returns which currency of measurement this size is in. */
  SizeKind kind() const noexcept { return kind_; }

  /**
   * Returns the quantity.
   *
   * @throws std::logic_error unless the size is a quantity.
   */
  const Decimal64& as_quantity() const {
    require(SizeKind::QUANTITY, "quantity");
    return quantity_;
  }

  /**
   * Returns the notional amount.
   *
   * @throws std::logic_error unless the size is a notional.
   */
  const Money& as_notional() const {
    require(SizeKind::NOTIONAL, "notional");
    return notional_;
  }

  /**
   * Returns the target weight.
   *
   * @throws std::logic_error unless the size is a target weight.
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
 * One order a strategy is asking for.
 *
 * A class rather than an aggregate, because it holds an invariant: a limit order carries a
 * price and a market order does not. The same rule exists on `OrderRequest`, where an adapter
 * enforces it before network I/O; stating it here as well means a strategy cannot even assemble
 * the invalid combination, so the failure arrives at the line that wrote it.
 *
 * `tag_` is the strategy's own label, carried through to the order the runner places so a fill
 * can be attributed back to the decision that caused it. Nothing in the library interprets it.
 */
class OrderIntent {
public:
  /** Returns a limit order for `size` at `limit`. */
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
   * Returns a market order for `size`.
   *
   * No price, and no way to attach one. A market order with a limit attached is a combination
   * every adapter rejects, so it is not constructible.
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

  /** Returns a copy labelled with `tag`, for attribution. */
  OrderIntent with_tag(std::string tag) const {
    OrderIntent copy = *this;
    copy.tag_ = std::move(tag);
    return copy;
  }

  /**
   * Returns a copy carrying a broker idempotency key.
   *
   * Only meaningful where `BrokerCaps<Client>::has_client_order_id` is true, which is why a
   * strategy sets it under an `if constexpr` on the context's capabilities rather than always.
   * A runner bound to a broker without one rejects the intent instead of dropping the key,
   * because a key silently discarded is a retry that looked safe and was not.
   */
  OrderIntent with_client_order_id(std::string key) const {
    OrderIntent copy = *this;
    copy.client_order_id_ = std::move(key);
    return copy;
  }

  /** Returns which instrument to trade. */
  const InstrumentId& instrument() const noexcept { return instrument_; }

  /** Returns which side to trade. */
  OrderSide side() const noexcept { return side_; }

  /** Returns whether this is a limit or a market order. */
  OrderType type() const noexcept { return type_; }

  /** Returns the validity condition. */
  TimeInForce time_in_force() const noexcept { return time_in_force_; }

  /** Returns how much to trade. */
  const SizeSpec& size() const noexcept { return size_; }

  /** Returns the limit price, present exactly when the order is a limit order. */
  const std::optional<Price>& limit() const noexcept { return limit_; }

  /** Returns the strategy's own label. */
  const std::string& tag() const noexcept { return tag_; }

  /** Returns the broker idempotency key, when the strategy supplied one. */
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
  std::string tag_;
  std::optional<std::string> client_order_id_;
};

/**
 * The position a strategy wants to hold, rather than the order to get there.
 *
 * **The form most strategies should emit.** The runner diffs a target against the current
 * position *and the orders already working*, and produces the minimum set of instructions. A
 * strategy that emits orders instead has to do that arithmetic itself, and the classic way it
 * gets it wrong is double-sizing after a partial fill: the signal still says "buy 100", 40 are
 * already filled and 60 still working, and a second order for 100 makes the position 200.
 * Stating the target makes that mistake unexpressible.
 *
 * `target_quantity_` is signed: negative is a short position, and zero is flat, which is how a
 * strategy asks to be taken out of an instrument entirely.
 */
struct TargetPosition {
  /** The instrument the target applies to. */
  InstrumentId instrument_;
  /** The position to hold, signed: negative is short, and zero asks to be flat. */
  Decimal64 target_quantity_;
  /** Optional limit for the orders the runner derives; a market order without one. */
  std::optional<Price> limit_;
  /** A label carried through to the orders and back on their fills, for attribution. */
  std::string tag_;
};

/**
 * A request to withdraw one working order.
 *
 * Templated on the broker because an `OrderRef` is: a reference is only meaningful to the
 * broker that issued it, and the compile-time tag is what stops one broker's reference being
 * passed to another's session.
 *
 * A strategy never writes the parameter. It takes a reference from the context with `auto` and
 * hands it to `ctx.cancel(...)`, so the strategy's source names no broker while the type system
 * still keeps the references apart:
 *
 * ```cpp
 * template <class Ctx>
 * void on_bar(Ctx& ctx, const BarEvent&) {
 *   for (const auto& working : ctx.portfolio().working_) {
 *     ctx.cancel(working.ref_);          // `auto` above, no broker named here
 *   }
 * }
 * ```
 */
template <typename Broker>
struct CancelIntent {
  /** The order to withdraw, taken from `ctx.portfolio()` with `auto`. */
  OrderRef<Broker> ref_;
  /** A label for attribution, as on `TargetPosition`. */
  std::string tag_;
};

} // namespace strategy
} // namespace algotradelib
