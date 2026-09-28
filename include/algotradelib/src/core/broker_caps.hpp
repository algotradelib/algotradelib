#pragma once

#include "./broker_traits.hpp"
#include "./calendar.hpp"
#include "./instrument.hpp"
#include "./market_data.hpp"

#include <type_traits>

namespace algotradelib {

namespace caps_detail {

/**
 * Whether a traits specialization states what its sellable *inquiry* answer is evidence of.
 *
 * **Detected with a default rather than required**, which is the opposite of the rule every other
 * constant here follows, and deliberately. Requiring it would make a traits specialization written
 * against an earlier release fail to compile on a member its author never heard of; detecting it
 * makes that adapter's checked sells *refuse*, at run time, with a message naming the declaration to
 * add. Both are discoverable and only the second leaves the rest of the adapter working -- and
 * neither one lets an unstated basis be enforced against, which is what matters.
 */
template <typename Traits, typename = void>
struct stated_inquiry_basis {
  static constexpr SellableBasis value{};
};

template <typename Traits>
struct stated_inquiry_basis<Traits, std::void_t<decltype(Traits::sellable_inquiry_basis)>> {
  static constexpr SellableBasis value = Traits::sellable_inquiry_basis;
};

}  // namespace caps_detail

/**
 * Compile-time feature set of one broker client.
 *
 * Each constant answers whether the broker *implements* an operation, which a
 * strategy gates on with `if constexpr` so one source file compiles against
 * every broker. Every specialization must answer every question explicitly; a
 * defaulted answer would let a new broker silently claim a feature it lacks.
 *
 * Capability is not availability. Whether a *configuration* may call an
 * implemented operation stays a run-time error: KIS implements the
 * modifiable-order inquiry but does not offer it to demo accounts, which is an
 * `ErrorCategory::INVALID_ARGUMENT` at run time. Encoding a per-credential
 * restriction here would make one binary claim different things for a live and a
 * demo key.
 */
template <typename Client>
struct BrokerCaps {
  /** Traits specialization these capabilities are read from. */
  using Traits = BrokerTraits<Client>;

  /** Whether the broker lists selectable accounts over the network. */
  static constexpr bool has_account_discovery = Traits::has_account_discovery;

  /** Whether the broker accepts a caller-supplied idempotency key on an order. */
  static constexpr bool has_client_order_id = Traits::has_client_order_id;

  /** Whether the broker publishes the orders still open to amendment. */
  static constexpr bool has_working_orders = Traits::has_working_orders;

  /** Whether the broker reports the quantity still amendable on an open order. */
  static constexpr bool has_amendable_quantity = Traits::has_amendable_quantity;

  /** Whether the broker retrieves one order by its identifier. */
  static constexpr bool has_order_by_id = Traits::has_order_by_id;

  /** Whether the broker can cancel part of an order rather than all of it. */
  static constexpr bool has_partial_cancel = Traits::has_partial_cancel;

  /** Whether an order-history inquiry requires an explicit calendar range. */
  static constexpr bool history_requires_range = Traits::history_requires_range;

  /** Whether an order-history inquiry exposes a caller-visible cursor. */
  static constexpr bool has_history_cursor = Traits::has_history_cursor;

  /**
   * Whether an order-history inquiry honours a caller-supplied row limit.
   *
   * False at KIS, which returns every page and *rejects* a limit rather than ignoring it. Code that
   * builds a history query for any broker must gate on this, or every KIS inquiry it makes fails
   * with `INVALID_ARGUMENT` before reaching the network.
   */
  static constexpr bool has_history_limit = Traits::has_history_limit;

  /** Whether an order-history inquiry can filter by instrument. */
  static constexpr bool has_history_symbol_filter = Traits::has_history_symbol_filter;

  /** Whether the broker publishes resting orders on both sides of the book. */
  static constexpr bool has_depth = Traits::has_depth;

  /** Whether the broker publishes today's upper and lower price bound. */
  static constexpr bool has_price_limits = Traits::has_price_limits;

  /** Whether the broker publishes recent executions. */
  static constexpr bool has_trade_prints = Traits::has_trade_prints;

  /** Whether the broker quotes several instruments in one request. */
  static constexpr bool has_batch_prices = Traits::has_batch_prices;

  /** Whether the broker answers what the account may spend. */
  static constexpr bool has_buying_power = Traits::has_buying_power;

  /**
   * Whether that answer includes how many shares are affordable, not only how much money there is.
   *
   * True at KIS, which prices a quantity against a specific instrument and order division, and
   * false at Toss, which reports one account-level amount. The port asks only for the amount,
   * because that is the question both answer; this is what points a caller at `session.client()`
   * for the rest.
   */
  static constexpr bool has_buying_power_quantity = Traits::has_buying_power_quantity;

  /**
   * Where this library learns what may be sold of a position.
   *
   * Deliberately a source rather than a `has_sellable_quantity` boolean. Every other constant here
   * answers whether the *broker* implements an operation, and by that question KIS's answer would
   * be "yes, and this library does not use it" -- it publishes 매도가능수량조회 and this library
   * reads the same figure from the holdings snapshot instead, for free and on demo accounts too.
   * Naming the source says what is true without implying a gap in the vendor's API, and it is what
   * a runner branches on: `HOLDINGS` costs nothing, `INQUIRY` costs one request per instrument sold.
   */
  static constexpr SellableSource sellable_source = Traits::sellable_source;

  /**
   * What a sellable figure obtained through the *inquiry* answers about.
   *
   * Only meaningful where `sellable_source` is `INQUIRY`: the figure arrives on its own, outside any
   * holdings read, so nothing else can say what it covers. Unestablished unless the traits state it,
   * so an adapter that answers an inquiry and characterizes nothing has its checked sells refused
   * rather than enforced against a number of unknown meaning.
   */
  static constexpr SellableBasis sellable_inquiry_basis =
    caps_detail::stated_inquiry_basis<Traits>::value;

  /** Whether the broker publishes the commission rates it charges the account. */
  static constexpr bool has_commission_schedule = Traits::has_commission_schedule;

  /** Whether the broker publishes static reference data for an instrument. */
  static constexpr bool has_reference_data = Traits::has_reference_data;

  /**
   * Whether one reference-data request covers several instruments.
   *
   * Toss takes up to two hundred symbols; KIS takes one product number. The port
   * asks for one instrument, which both can answer, and this says whether a caller
   * building a universe can do it in one request through `session.client()`.
   */
  static constexpr bool has_batch_reference_data = Traits::has_batch_reference_data;

  /** Whether the broker publishes which dates the market trades on. */
  static constexpr bool has_calendar = Traits::has_calendar;

  /**
   * Whether one calendar request covers a span of dates rather than a few.
   *
   * KIS answers from a base date onwards; Toss answers with the requested day and
   * the business day either side. A caller that needs a month of dates can get it
   * in one request from one broker and not the other, which is worth knowing before
   * writing the loop.
   */
  static constexpr bool has_calendar_range = Traits::has_calendar_range;

  /**
   * Whether the broker publishes intraday session windows.
   *
   * Only Toss does, and they are deliberately not normalized, so this capability
   * exists to point a caller at `session.client()` rather than to gate a port verb.
   * Gating broker-specific work on it is the intended use.
   */
  static constexpr bool has_calendar_sessions = Traits::has_calendar_sessions;

  /**
   * Whether the broker publishes bars at `interval`.
   *
   * A predicate rather than a constant, because the answer differs per interval.
   * Both brokers publish one-minute and one-day bars; only KIS publishes weekly,
   * monthly, and yearly ones. An interval a broker does not publish is refused,
   * never produced by aggregating a shorter one, so a strategy that wants a weekly
   * bar gates on this and knowingly binds itself to the broker that has them.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    return Traits::supports_interval(interval);
  }
};

} // namespace algotradelib
