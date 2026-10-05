#pragma once

#include "./broker_traits.hpp"
#include "./calendar.hpp"
#include "./instrument.hpp"
#include "./market_data.hpp"

#include <type_traits>

namespace algotradelib {

namespace caps_detail {

/**
 * Reads `T::working_orders_state_terminal` where `T::has_working_orders` is true, and answers false where
 * it is not: the question is asked only of a broker that lists working orders, so only that broker must
 * answer it. `T` is a traits specialization or a hand-written capability set.
 */
template <typename T>
constexpr bool terminal_working_state() {
  if constexpr (T::has_working_orders) {
    return T::working_orders_state_terminal;
  } else {
    return false;
  }
}

/**
 * Reads `T::sellable_inquiry_basis` where `T::sellable_source` is `INQUIRY`, and answers an unestablished
 * basis where it is not: no inquiry is made there, so no figure needs one. `T` is a traits
 * specialization or a hand-written capability set.
 */
template <typename T>
constexpr SellableBasis inquiry_basis() {
  if constexpr (T::sellable_source == SellableSource::INQUIRY) {
    return T::sellable_inquiry_basis;
  } else {
    return SellableBasis{};
  }
}

}  // namespace caps_detail

/**
 * Compile-time feature set of one broker client.
 *
 * Each constant answers whether the broker *implements* an operation, which a
 * strategy gates on with `if constexpr` so one source file compiles against
 * every broker. Every specialization must answer every question explicitly, and
 * none has a default: a `false` is not always the safe answer (one for
 * `has_history_cursor` reads a first page as the whole history; one for
 * `has_working_orders` stops a runner keeping the orders it sent), so a missing
 * answer does not compile rather than being chosen for the adapter. Two questions
 * are asked only of a broker that makes the inquiry they are about, and only that
 * broker answers them: `working_orders_state_terminal` and `sellable_inquiry_basis`.
 * `ATL_ASSERT_BROKER` reads every one, so an adapter's own test finds an omission.
 *
 * Capability is not availability. Whether a *configuration* may call an
 * implemented operation stays a run-time matter: KIS does not offer its
 * modifiable-order inquiry to demo accounts, so its adapter answers that read
 * from the daily order inquiry there instead. Encoding a per-credential
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

  /**
   * Whether a row in that listing can report a *terminal* status, so an order's outcome is
   * observable without a second inquiry.
   *
   * **False on all three shipped adapters**, and the constant exists for the adapters this library
   * has never seen rather than to describe a difference between KIS, Toss and Kiwoom. KIS's
   * 정정취소가능주문조회 derives `PENDING` or `PARTIAL_FILLED` from the filled quantity and can
   * state nothing else; Toss requests the `OPEN` lifecycle group, whose terminal counterpart is
   * `CLOSED`; Kiwoom's `ka10075` is the 미체결 listing and its specification documents `ord_stt`
   * with the single value `접수`.
   *
   * **What `false` means for a caller, exactly.** `runner::OrderUpdateWatch` reports what a listed
   * row says and never interprets a row's *disappearance*, because a listing cannot say whether an
   * order filled, was cancelled, expired or was rejected. So under `false` a strategy must not wait
   * for a terminal `on_order_update`: it will not arrive, and the order's outcome has to be read
   * from holdings or an order inquiry. Under `true` it may.
   *
   * This is a statement about the *inquiry*, not about the adapter's parser. A parser able to
   * represent a terminal status proves only what it would do with such a row, never that the
   * endpoint supplies one, and answering `true` on that evidence would strand a caller waiting.
   *
   * Required where `has_working_orders` is true, and `false` here where it is not.
   */
  static constexpr bool working_orders_state_terminal = caps_detail::terminal_working_state<Traits>();

  /**
   * Whether the adapter transmits `OrderRequest::trigger_price_` and `OrderShape::trigger_price_`.
   *
   * An adapter that does not read `trigger_price_` declares `false`. `BrokerSession` then refuses a
   * placement or a modification carrying a trigger with `unsupported_trigger_price`, before the
   * adapter runs, rather than sending a stop order without its releasing condition. A cancellation
   * is never refused for one -- see `BrokerSession::cancel`.
   *
   * `true` at KIS and Kiwoom, `false` at Toss, which publishes no stop basis.
   */
  static constexpr bool has_trigger_price = Traits::has_trigger_price;

  /**
   * Whether the client is a market-data source that takes no order: true for a traits type built on
   * `DataOnlyTraits`.
   *
   * **Read at compile time where the library binds a session to send or to read an account:**
   * `live::SessionExecutor` and the trading (first) session of `live::SessionSource` refuse such a client
   * with a `static_assert`, so a data vendor wired there does not build (`live::bind`, `bind_journaled` and
   * `live_program` reach both). A caller's direct `place` or `holdings` on the session is not checked; it
   * refuses at run time.
   *
   * Declared as `static constexpr bool data_only` by every traits specialization: `false` for a broker,
   * `true` through `DataOnlyTraits`. A hand-written capability set a session publishes (`Session::Caps`)
   * declares it too, since both binding checks read it directly; an unstated one does not build.
   */
  static constexpr bool data_only = Traits::data_only;

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
   * holdings read, so nothing else can say what it covers. Required where `sellable_source` is
   * `INQUIRY`, and unestablished here where it is not. An adapter that makes the inquiry and has not
   * established what the figure covers states `SellableBasis{}`, and its checked sells are refused
   * rather than enforced against a number of unknown meaning.
   */
  static constexpr SellableBasis sellable_inquiry_basis = caps_detail::inquiry_basis<Traits>();

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

/**
 * Reads every capability constant of `Caps` once -- `BrokerCaps<Client>`, or a capability set written by
 * hand such as `strategy::NoCaps` -- so a missing one fails here rather than where a caller first reads
 * it. Always true, whatever the answers are.
 *
 * Needed because instantiating `BrokerCaps<Client>` does not evaluate its constants: each is evaluated
 * where it is read, and a constant nothing in the adapter's own test reads would otherwise surface in a
 * consumer's build. A constant added to `BrokerCaps` is added here.
 */
template <typename Caps>
constexpr bool reads_every_capability() {
  constexpr bool read[] = {
    Caps::has_account_discovery,
    Caps::has_client_order_id,
    Caps::has_working_orders,
    Caps::has_amendable_quantity,
    Caps::working_orders_state_terminal,
    Caps::has_trigger_price,
    Caps::data_only,
    Caps::has_order_by_id,
    Caps::has_partial_cancel,
    Caps::history_requires_range,
    Caps::has_history_cursor,
    Caps::has_history_limit,
    Caps::has_history_symbol_filter,
    Caps::has_depth,
    Caps::has_price_limits,
    Caps::has_trade_prints,
    Caps::has_batch_prices,
    Caps::has_buying_power,
    Caps::has_buying_power_quantity,
    Caps::sellable_source == SellableSource::INQUIRY,
    Caps::sellable_inquiry_basis.coverage_ == SellableBasis::Coverage::UNESTABLISHED,
    Caps::has_commission_schedule,
    Caps::has_reference_data,
    Caps::has_batch_reference_data,
    Caps::has_calendar,
    Caps::has_calendar_range,
    Caps::has_calendar_sessions,
  };
  static_cast<void>(read);
  // Named, not called: the predicate need not be usable in a constant expression to be conforming.
  static_cast<void>(&Caps::supports_interval);
  return true;
}

/** Whether `Client`'s traits answer every capability question; `ATL_ASSERT_BROKER` reads it. */
template <typename Client>
constexpr bool answers_every_capability() {
  return reads_every_capability<BrokerCaps<Client>>();
}

} // namespace algotradelib
