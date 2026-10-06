#pragma once

/**
 * @brief Defined as 1 once a broker adapter header is included.
 *
 * The library's tests use it to check that the strategy headers include no broker.
 */
#define ATL_BROKER_ADAPTER_PRESENT 1

#include "./client.hpp"

#include "../../core/broker_session.hpp"
#include "../../core/broker_traits.hpp"

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>

#include <optional>
#include <string>
#include <utility>

namespace algotradelib {

namespace kiwoom {

/**
 * @brief The Kiwoom broker tag: order references are typed by it, so a Kiwoom reference works with
 *   both Kiwoom clients and with no other broker.
 */
struct Broker {
  /** @brief The broker's name. */
  static constexpr std::string_view name = "kiwoom";
};

namespace traits_detail {

/** @brief A Kiwoom order reference (`primary_` the order number, `secondary_` the symbol). */
using Ref = OrderRef<kiwoom::Broker>;

/** @brief The capabilities and conversions shared by Kiwoom's asynchronous and blocking traits. */
struct Normalizers {
  /** @brief Kiwoom publishes ten levels each side, through `ka10004`. */
  static constexpr bool has_depth = true;

  /** @brief Kiwoom publishes both bounds on its `ka10007` 시세표성정보 response. */
  static constexpr bool has_price_limits = true;

  /** @brief Kiwoom publishes recent executions, through `ka10003`. */
  static constexpr bool has_trade_prints = true;

  /** @brief Every Kiwoom market-data endpoint takes one `stk_cd`. */
  static constexpr bool has_batch_prices = false;

  /** @brief Kiwoom answers what may be spent, in won, from `kt00001` 예수금상세현황요청. */
  static constexpr bool has_buying_power = true;

  /**
   * @brief `true`: Kiwoom also answers how many shares are affordable, per margin tier; reach it
   *   through `session.client()` (`KiwoomClient::async_orderable_amount`).
   */
  static constexpr bool has_buying_power_quantity = true;

  /**
   * @brief `NONE`: there is no per-instrument sellable inquiry. The holdings rows carry
   *   `trde_able_qty`, enforced only as far as `KiwoomConfig::holdings_sellable_scope_` asserts.
   */
  static constexpr SellableSource sellable_source = SellableSource::NONE;

  /**
   * @brief `false`: Kiwoom publishes no commission rates (it reports commission and tax charged per
   *   holding and per order, which is a different figure).
   */
  static constexpr bool has_commission_schedule = false;

  /** @brief Kiwoom publishes static reference data, through `ka10100` 종목정보 조회. */
  static constexpr bool has_reference_data = true;

  /** @brief `false`: one instrument per reference-data request. */
  static constexpr bool has_batch_reference_data = false;

  /**
   * @brief `false`: Kiwoom publishes no trading calendar, so a runner bound to Kiwoom never
   *   establishes a market phase (and reports so).
   */
  static constexpr bool has_calendar = false;

  /** @brief There is no calendar, so there is no range. */
  static constexpr bool has_calendar_range = false;

  /** @brief There is no calendar, so there are no session windows. */
  static constexpr bool has_calendar_sessions = false;

  /**
   * @brief Refuses every calendar request (Kiwoom has no calendar).
   * @param country The market.
   * @param from The date.
   * @return An `INVALID_ARGUMENT` error (not sent).
   */
  static std::optional<BrokerError> validate_calendar(Country country, std::optional<Date> from) {
    static_cast<void>(country);
    static_cast<void>(from);
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "calendar_unsupported";
    error.message_ = "Kiwoom publishes no trading calendar";
    return error;
  }

  /**
   * @brief Tests whether Kiwoom publishes bars at an interval.
   * @param interval The interval.
   * @return `true`: Kiwoom publishes every interval.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    static_cast<void>(interval);
    return true;
  }

  /**
   * @brief Refuses a bar request this broker cannot answer, before any I/O.
   * @param query The query.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return kiwoom::unsupported_bar_interval(query.interval_);
  }

  /**
   * @brief Makes the session's holdings refusal, used while `KiwoomConfig::holdings_position_scope_`
   *   is not stated.
   *
   * One `kt00018` request names one venue, and Kiwoom does not document whether that filters the
   * positions or only values them, so the session will not present it as the account's holdings
   * until you assert which. The client's own holdings request (`async_holdings`, or `holdings()` on
   * the blocking client) still returns the rows for the configured venue.
   *
   * @return An `INVALID_ARGUMENT` error (`holdings_scope_unestablished`, not sent).
   */
  static Result<HoldingsSnapshot> refuse_holdings() {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "holdings_scope_unestablished";
    error.message_ =
      "Kiwoom's balance inquiry names one venue (kt00018 dmst_stex_tp: KRX or NXT) and publishes no "
      "all-venue value, and whether that filters the positions or only values them is not stated. "
      "This library will not present one venue's answer as the account's holdings unless you say "
      "that is what it is: set KiwoomConfig::holdings_position_scope_ to "
      "KiwoomPositionScope::VALUATION_ONLY to assert it, and "
      "KiwoomConfig::holdings_sellable_scope_ separately for trde_able_qty. Until then read the rows "
      "through session.client().holdings(). Both assertions are unverified; the experiment that "
      "settles them is in issues/kiwoom-api-holdings-venue-scope.md";
    return Result<HoldingsSnapshot>(std::move(error));
  }

  /**
   * @brief Makes an order reference from Kiwoom's: `primary_` the order number, `secondary_` the
   *   symbol (amendments and cancellations need both).
   * @param order Kiwoom's reference.
   * @return The reference.
   */
  static Ref make_ref(const KiwoomOrderRef& order) {
    Ref ref;
    ref.primary_ = order.order_number_;
    ref.secondary_ = order.symbol_;
    return ref;
  }

  /**
   * @brief Returns Kiwoom's reference from an order reference.
   * @param ref The reference.
   * @return Kiwoom's reference.
   */
  static KiwoomOrderRef from_ref(const Ref& ref) {
    KiwoomOrderRef order;
    order.order_number_ = ref.primary_;
    order.symbol_ = ref.secondary_;
    return order;
  }

  /**
   * @brief Refuses an order Kiwoom cannot place as described (one with a client order id; see
   *   `kiwoom::unsupported_client_order_id`).
   * @param order The order.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    // **The holdings refusal is deliberately not repeated here**, although this is the port's
    // pre-flight. It lives in `KiwoomClient::validate_order`, which every placement reaches --
    // including this port's, because `place` calls the client immediately after this returns and
    // the client refuses before any I/O. Checking it here as well would only change *which* message
    // a caller gets: this function does not validate the order's venue, price or shape, so it would
    // report the holdings gap for an order that is also malformed, burying the mistake the caller
    // can actually fix. See `kiwoom::unestablished_holdings_scope`.
    return kiwoom::unsupported_client_order_id(order);
  }

  /**
   * @brief Normalizes a Kiwoom acknowledgement into an acknowledgement and a reference.
   * @param raw The client's result.
   * @return The result, or an error.
   */
  static Result<PlacedOrder<kiwoom::Broker>> normalize_place(Result<KiwoomOrderAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<kiwoom::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<kiwoom::Broker> placed;
    placed.ack_ = std::move(raw.value().ack_);
    placed.ref_ = make_ref(raw.value().order_);
    return Result<PlacedOrder<kiwoom::Broker>>(std::move(placed));
  }

  /**
   * @brief Converts Kiwoom's unfilled-order list into working orders.
   *
   * Each reference is the row's own order number (`ord_no`), not `orig_ord_no` (the order an
   * amendment replaced; see `KiwoomOpenOrder::original_order_number_`).
   *
   * @param raw The client's result.
   * @return The working orders, or the error.
   */
  static Result<WorkingOrders<kiwoom::Broker>> normalize_working_orders(
    Result<KiwoomOpenOrderPage> raw
  ) {
    if (!raw) {
      return Result<WorkingOrders<kiwoom::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<kiwoom::Broker> orders;
    orders.received_at_ = raw.value().received_at_;
    orders.orders_.reserve(raw.value().orders_.size());
    for (auto& open_order : raw.value().orders_) {
      WorkingOrder<kiwoom::Broker> working;
      working.order_ = std::move(open_order.order_);
      working.ref_ = make_ref(open_order.order_ref_);
      working.amendable_quantity_ = open_order.amendable_quantity_;
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<kiwoom::Broker>>(std::move(orders));
  }

  /**
   * @brief Converts an amendment or cancellation into Kiwoom's form.
   *
   * Kiwoom cannot change an order's type, condition or session, so an instruction asking to is
   * refused (a stated value equal to the resting one is not a change; see `refuse_shape_change`).
   * Also refused: no `quantity_` and not `all_remaining_` (`quantity_required`), and on a
   * modification a trigger price that does not fit the type (`invalid_trigger_price`). "All remaining" is
   * sent as quantity `0`, which Kiwoom evaluates when it processes the instruction.
   * `confirm_high_value_` is ignored.
   *
   * @param ref The order's reference.
   * @param instruction The instruction.
   * @param cancel Whether it is a cancellation.
   * @return Kiwoom's amendment, or an `INVALID_ARGUMENT` error.
   */
  static Result<KiwoomAmendment> to_amendment(
    const Ref& ref,
    const AmendInstruction& instruction,
    bool cancel
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;

    if (!instruction.all_remaining_ && !instruction.quantity_) {
      error.broker_code_ = "quantity_required";
      error.message_ =
        "a Kiwoom modification or cancellation affects a stated quantity; set all_remaining_ to "
        "act on everything still resting, or build the instruction with cancel_all()";
      return Result<KiwoomAmendment>(std::move(error));
    }
    if (auto refusal = refuse_shape_change(instruction, cancel)) {
      return Result<KiwoomAmendment>(std::move(*refusal));
    }

    KiwoomAmendment amendment;
    amendment.order_ = from_ref(ref);
    amendment.venue_ = instruction.venue_;
    // Kiwoom's own "everything still resting", which it evaluates when it processes the
    // instruction rather than when the caller read the listing.
    amendment.quantity_ =
      instruction.all_remaining_ ? Decimal64() : *instruction.quantity_;
    if (!cancel) {
      // The basis is known here and nowhere after: `kt10002` carries no `trde_tp`, so this is the
      // last point that can tell a trigger the order wants from one it would silently lose.
      const auto trigger = amended_trigger_price(instruction);
      if (auto invalid = kiwoom::invalid_trigger(instruction.shape_.type_, trigger)) {
        return Result<KiwoomAmendment>(std::move(*invalid));
      }
      amendment.price_ = instruction.shape_.price_;
      amendment.condition_price_ = trigger;
    }
    // **Never on a cancellation.** `kt10003` has no trigger field and the client refuses a
    // cancellation carrying one; `cancel_all` restates the resting trigger, so copying it here would
    // make every stop order impossible to withdraw through the port.
    return Result<KiwoomAmendment>(std::move(amendment));
  }

  /**
   * @brief Refuses an instruction that asks to change what a Kiwoom amendment cannot carry (type,
   *   condition, session, or a price or trigger price on a cancellation).
   *
   * An unset field means "unchanged"; a set field is a change if it differs from the resting shape.
   * A modification without `resting_` is refused (`incomplete_shape`) even if it sets no shape
   * field; `reprice()` and `replace_with()` supply it.
   *
   * @param instruction The instruction.
   * @param cancel Whether it is a cancellation.
   * @return An `INVALID_ARGUMENT` error, or `std::nullopt`.
   */
  static std::optional<BrokerError> refuse_shape_change(
    const AmendInstruction& instruction,
    bool cancel
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "shape_change_unsupported";

    const auto refuse = [&error, cancel](const char* what) {
      error.message_ = cancel ?
        std::string("Kiwoom's cancellation carries a quantity and nothing else, so it cannot "
                    "change an order's ") + what + " on the way out" :
        std::string("Kiwoom's amendment endpoints carry a quantity and a price only, so they "
                    "cannot change an order's ") + what +
          "; cancel the order and place a new one instead";
      return error;
    };

    // An unset field means "leave it as it is", so it is never a change. A set field is a change
    // only when it differs from what the order already is -- and a set field with no resting shape
    // to compare against cannot be shown to be a restatement, so it is refused rather than assumed
    // harmless.
    const auto differs = [](auto stated, auto current) {
      return stated.has_value() && (!current.has_value() || *stated != *current);
    };

    if (cancel) {
      // **A cancellation transmits a quantity and nothing else**, so every other stated field is
      // one Kiwoom cannot act on. The price and the trigger are refused because acting on them is
      // what a caller would expect; these three are refused for the same reason and it is not a
      // lesser one -- reporting acceptance for a stated change that never reached the broker is
      // the defect, whatever the field.
      //
      // `cancel_all()` and `cancel_quantity()` copy the resting order's whole shape into both
      // `shape_` and `resting_`, so every comparison below sees a restatement and the ordinary
      // withdrawal path is unaffected.
      const bool restates_price = instruction.resting_ &&
        instruction.resting_->price_ == instruction.shape_.price_;
      if (instruction.shape_.price_ && !restates_price) {
        error.broker_code_ = "price_on_cancellation";
        error.message_ =
          "Kiwoom's cancellation carries a quantity and no price, so it cannot reprice an order on "
          "the way out; cancel it and place a new one instead";
        return error;
      }
      // The same rule as the price: a restatement of the resting trigger -- which is what
      // `cancel_all` writes -- is the ordinary withdrawal, and only a stated *change* is refused.
      const bool restates_trigger = instruction.resting_ &&
        instruction.resting_->trigger_price_ == instruction.shape_.trigger_price_;
      if (instruction.shape_.trigger_price_ && !restates_trigger) {
        error.broker_code_ = "trigger_on_cancellation";
        error.message_ =
          "Kiwoom's cancellation carries a quantity and no trigger price, so it cannot move an "
          "order's stop on the way out; cancel it and place a new one instead";
        return error;
      }
    } else if (!instruction.resting_) {
      // **A modification with no resting shape is refused, and that is the conservative
      // direction.** `OrderShape::type_` is a plain value defaulting to `LIMIT`, so this adapter
      // cannot tell a caller who *means* `LIMIT` from one who left it alone -- and if the resting
      // order is a best-limit order, accepting the instruction would send a price and leave the
      // basis where it was, reporting success for a change that did not happen. KIS refuses the
      // same case for the same reason, as `incomplete_shape`.
      error.broker_code_ = "incomplete_shape";
      error.message_ =
        "Kiwoom's modification cannot change an order's price basis, validity condition or session, "
        "and without the order's current shape this adapter cannot tell a restatement from a "
        "change; build the instruction with reprice() or replace_with(), which take it from the "
        "resting order";
      return error;
    }

    const auto resting_condition = instruction.resting_ ?
      instruction.resting_->time_in_force_ :
      std::optional<TimeInForce>();
    const auto resting_session = instruction.resting_ ?
      instruction.resting_->session_ :
      std::optional<OrderSession>();
    if (differs(instruction.shape_.time_in_force_, resting_condition)) {
      return refuse("validity condition");
    }
    if (differs(instruction.shape_.session_, resting_session)) {
      return refuse("session");
    }
    // `OrderShape::type_` is a plain value rather than an optional, so "left alone" and "stated
    // `LIMIT`" are the same bytes and the comparison has to be made in two steps.
    if (instruction.resting_) {
      if (instruction.shape_.type_ != instruction.resting_->type_) {
        return refuse("price basis");
      }
    } else if (instruction.shape_.type_ != OrderShape().type_) {
      // **With no resting shape, a value that differs from the default is still a stated change.**
      // Only the default is ambiguous; `MARKET` on a cancellation is a caller saying something, and
      // `kt10003` transmits no basis, so accepting it would report acceptance for a change the
      // broker never saw. Compared against `OrderShape`'s own default rather than a literal, so the
      // two cannot drift apart.
      return refuse("price basis");
    }
    return std::nullopt;
  }

  /**
   * @brief Converts a normalized modification into Kiwoom's own amendment.
   * @param ref The order's reference.
   * @param instruction The amendment or cancellation.
   * @return The result, or an error.
   */
  static Result<KiwoomAmendment> to_replacement(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction, false);
  }

  /**
   * @brief Converts a normalized cancellation into Kiwoom's own amendment.
   * @param ref The order's reference.
   * @param instruction The amendment or cancellation.
   * @return The result, or an error.
   */
  static Result<KiwoomAmendment> to_cancellation(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction, true);
  }

  /**
   * @brief Converts an order-history query into Kiwoom's single-date inquiry (`kt00007`).
   *
   * Refused: no `from_` (`date_required`), a range of more than one day (ask once per day), a
   * `cursor_`, or a `limit_`. `symbol_` is passed to Kiwoom as its `stk_cd` filter. The account
   * argument is not used.
   *
   * @param query The query.
   * @return The inquiry, or an `INVALID_ARGUMENT` error.
   */
  static Result<KiwoomHistoryQuery> to_history_query(
    const KiwoomAccount&,
    const OrderHistoryQuery& query
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (!query.from_) {
      error.broker_code_ = "date_required";
      error.message_ = "Kiwoom's order-history inquiry lists one calendar date, which must be given";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    if (query.to_ && !(query.to_->year_ == query.from_->year_ &&
                       query.to_->month_ == query.from_->month_ &&
                       query.to_->day_ == query.from_->day_)) {
      error.broker_code_ = "single_date_only";
      error.message_ =
        "Kiwoom's order-history inquiry lists one calendar date; ask for each date separately "
        "rather than for a range";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    if (query.cursor_) {
      error.broker_code_ = "cursor_unsupported";
      error.message_ = "Kiwoom order history pages internally and exposes no cursor";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    if (query.limit_) {
      error.broker_code_ = "limit_unsupported";
      error.message_ = "Kiwoom order history documents no row limit and cannot honor one";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    KiwoomHistoryQuery converted;
    converted.date_ = *query.from_;
    converted.symbol_ = query.symbol_ ? *query.symbol_ : std::string();
    return Result<KiwoomHistoryQuery>(std::move(converted));
  }

  /**
   * @brief Returns the account selector a session binds, which is empty (the credential is the
   *   account; see `KiwoomAccount`). The argument is not used.
   * @return An empty selector.
   */
  static KiwoomAccount selector_of(const KiwoomAccountInfo&) { return KiwoomAccount{}; }

  /**
   * @brief Returns the account number as Kiwoom displays it.
   * @param account The account.
   * @return The number.
   */
  static std::string display_of(const KiwoomAccountInfo& account) {
    return account.account_number_;
  }

  /**
   * @brief Returns the account type, which Kiwoom does not publish (the argument is not used).
   * @return An empty string.
   */
  static std::string type_of(const KiwoomAccountInfo&) { return {}; }
};

/** @brief The order-related capabilities shared by Kiwoom's asynchronous and blocking traits. */
struct Capabilities {
  /** @brief `true`: Kiwoom reports the one account its credential drives (`ka00001`). */
  static constexpr bool has_account_discovery = true;

  /** @brief Kiwoom documents no caller-supplied order identifier. */
  static constexpr bool has_client_order_id = false;

  /**
   * @brief Kiwoom sends a stop-limit (`28`) order's trigger as `cond_uv`, and a modified one as
   *   `mdfy_cond_uv`.
   */
  static constexpr bool has_trigger_price = true;

  /** @brief A broker that takes orders, not a market-data source; see `BrokerCaps::data_only`. */
  static constexpr bool data_only = false;

  /** @brief Kiwoom publishes its unfilled orders, through `ka10075`. */
  static constexpr bool has_working_orders = true;

  /** @brief Kiwoom reports `oso_qty` 미체결수량, which is what remains amendable. */
  static constexpr bool has_amendable_quantity = true;

  /**
   * @brief `false`: the unfilled-order listing (`ka10075`) shows only open orders; read an order's
   *   outcome from the history.
   */
  static constexpr bool working_orders_state_terminal = false;

  /** @brief `false`: no inquiry of one order by number. */
  static constexpr bool has_order_by_id = false;

  /** @brief Kiwoom cancels a stated quantity through `kt10003`'s `cncl_qty`. */
  static constexpr bool has_partial_cancel = true;

  /** @brief `false`: no range, but one date (`from_`) is required; see `to_history_query`. */
  static constexpr bool history_requires_range = false;

  /** @brief Kiwoom follows its own `cont-yn` / `next-key` continuation and exposes none. */
  static constexpr bool has_history_cursor = false;

  /** @brief Kiwoom documents no row limit on an order-history inquiry. */
  static constexpr bool has_history_limit = false;

  /** @brief `kt00007` takes `stk_cd`, so the filter is the broker's rather than applied locally. */
  static constexpr bool has_history_symbol_filter = true;
};

} // namespace traits_detail
} // namespace kiwoom

/**
 * @brief Returns the venue a Kiwoom order reaches: the one it names. Kiwoom has no default venue, so
 *   an order naming none is refused before sending.
 * @param asked The order's venue.
 * @return `asked`.
 */
inline constexpr Venue kiwoom_route(Venue asked) noexcept { return kiwoom::kiwoom_effective_route(asked); }

/** @brief The `BrokerSession` adapter for `kiwoom::KiwoomClient`. */
template <>
struct BrokerTraits<kiwoom::KiwoomClient>
: kiwoom::traits_detail::Normalizers, kiwoom::traits_detail::Capabilities {
  /**
   * @brief Returns the venue an order reaches; see `kiwoom_route`.
   * @param asked The order's venue.
   * @return The venue.
   */
  static constexpr Venue effective_route(Venue asked) noexcept { return kiwoom_route(asked); }

  /** @brief The client type. */
  using Client = kiwoom::KiwoomClient;

  /** @brief Kiwoom application credentials and environment selection. */
  using Config = kiwoom::KiwoomConfig;

  /** @brief Kiwoom account selector, which carries nothing because Kiwoom takes nothing. */
  using Account = kiwoom::KiwoomAccount;

  /** @brief Item type of the broker's own account listing. */
  using AccountListItem = kiwoom::KiwoomAccountInfo;

  /** @brief The broker tag. */
  using BrokerTag = kiwoom::Broker;

  /** @brief Acknowledgement Kiwoom returns for an order instruction. */
  using PlaceAck = kiwoom::KiwoomOrderAck;

  /** @brief Kiwoom modification and cancellation instruction. */
  using Amendment = kiwoom::KiwoomAmendment;

  /** @brief Kiwoom order-history selector. */
  using HistoryQuery = kiwoom::KiwoomHistoryQuery;

  /** @brief Kiwoom amendable-order result. */
  using WorkingOrdersResult = kiwoom::KiwoomOpenOrderPage;

  /** @brief The broker's name. */
  static constexpr std::string_view name = kiwoom::Broker::name;

  /** @brief `false`: requests complete through a completion token. */
  static constexpr bool blocking = false;

  /**
   * @brief Returns the rate limiter's statistics.
   * @param client The client.
   * @return The statistics, or null if the client has no rate-limit budget.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /**
   * @brief Forwards a quote request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a price-bound request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto price_limits(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_price_limits(std::move(instrument), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a recent-execution request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param count The most trades to return.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto trades(Client& client, InstrumentId instrument, int count, Token&& token) {
    return client.async_trades(std::move(instrument), count, std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order book request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto depth(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_depth(std::move(instrument), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a bar request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param query The query.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto bars(Client& client, BarQuery query, Token&& token) {
    return client.async_bars(std::move(query), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a reference-data request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param subject The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto instrument(Client& client, InstrumentId subject, Token&& token) {
    return client.async_instrument(std::move(subject), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an account listing request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto accounts(Client& client, Token&& token) {
    return client.async_accounts(std::forward<Token>(token));
  }

  /**
   * @brief Forwards a buying-power request for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param currency The currency (only KRW is answered).
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto buying_power(
    Client& client,
    const Account& account,
    Currency currency,
    Token&& token
  ) {
    return client.async_buying_power(currency, account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards a holdings request, or completes it with `refuse_holdings()` (sending nothing)
   *   while `KiwoomConfig::holdings_position_scope_` is not stated.
   *
   * The refusal completes on the handler's associated executor, like any other completion.
   *
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto holdings(Client& client, const Account& account, Token&& token) {
    if (client.config().holdings_position_scope_ == kiwoom::KiwoomPositionScope::UNSPECIFIED) {
      static_cast<void>(account);
      return net::async_initiate<Token, void(Result<HoldingsSnapshot>)>(
        [strand = client.get_executor()](auto&& handler) mutable {
          auto work = net::prefer(
            net::get_associated_executor(handler), net::execution::outstanding_work.tracked
          );
          net::post(
            strand,
            [handler = std::forward<decltype(handler)>(handler), work = std::move(work)]() mutable {
              net::dispatch(work, [handler = std::move(handler)]() mutable {
                std::move(handler)(refuse_holdings());
              });
            }
          );
        },
        token
      );
    }
    return client.async_holdings(account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order submission for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param order The order.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto place_order(
    Client& client,
    const Account& account,
    OrderRequest order,
    Token&& token
  ) {
    return client.async_place_order(account, std::move(order), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order modification for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto replace_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_replace_order(account, std::move(amendment), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order cancellation for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_cancel_order(account, std::move(amendment), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an amendable-order inquiry for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto working_orders(Client& client, const Account& account, Token&& token) {
    return client.async_open_orders(account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order-history inquiry for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param query The query.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto order_history(
    Client& client,
    const Account& account,
    HistoryQuery query,
    Token&& token
  ) {
    return client.async_order_history(account, std::move(query), std::forward<Token>(token));
  }
};

/** @brief The `BlockingSession` adapter for `kiwoom::KiwoomBlockingClient`. */
template <>
struct BrokerTraits<kiwoom::KiwoomBlockingClient>
: kiwoom::traits_detail::Normalizers, kiwoom::traits_detail::Capabilities {
  /**
   * @brief Returns the venue an order reaches; see `kiwoom_route`.
   * @param asked The order's venue.
   * @return The venue.
   */
  static constexpr Venue effective_route(Venue asked) noexcept { return kiwoom_route(asked); }

  /** @brief The client type. */
  using Client = kiwoom::KiwoomBlockingClient;

  /** @brief Kiwoom application credentials and environment selection. */
  using Config = kiwoom::KiwoomConfig;

  /** @brief Kiwoom account selector, which carries nothing because Kiwoom takes nothing. */
  using Account = kiwoom::KiwoomAccount;

  /** @brief Item type of the broker's own account listing. */
  using AccountListItem = kiwoom::KiwoomAccountInfo;

  /** @brief The broker tag. */
  using BrokerTag = kiwoom::Broker;

  /** @brief Acknowledgement Kiwoom returns for an order instruction. */
  using PlaceAck = kiwoom::KiwoomOrderAck;

  /** @brief Kiwoom modification and cancellation instruction. */
  using Amendment = kiwoom::KiwoomAmendment;

  /** @brief Kiwoom order-history selector. */
  using HistoryQuery = kiwoom::KiwoomHistoryQuery;

  /** @brief Kiwoom amendable-order result. */
  using WorkingOrdersResult = kiwoom::KiwoomOpenOrderPage;

  /** @brief The broker's name. */
  static constexpr std::string_view name = kiwoom::Broker::name;

  /** @brief `true`: requests return their result directly. */
  static constexpr bool blocking = true;

  /**
   * @brief Returns the rate limiter's statistics.
   * @param client The client.
   * @return The statistics, or null if the client has no rate-limit budget.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /**
   * @brief Returns a quote.
   * @param client The client.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) {
    return client.quote(std::move(instrument));
  }

  /**
   * @brief Returns today's price bounds.
   * @param client The client.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<PriceLimits> price_limits(Client& client, InstrumentId instrument) {
    return client.price_limits(std::move(instrument));
  }

  /**
   * @brief Returns recent executions.
   * @param client The client.
   * @param instrument The instrument.
   * @param count The most trades to return.
   * @return The result, or an error.
   */
  static Result<std::vector<TradePrint>> trades(
    Client& client,
    InstrumentId instrument,
    int count
  ) {
    return client.trades(std::move(instrument), count);
  }

  /**
   * @brief Returns the order book.
   * @param client The client.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<DepthSnapshot> depth(Client& client, InstrumentId instrument) {
    return client.depth(std::move(instrument));
  }

  /**
   * @brief Returns bars, paging inside the client.
   * @param client The client.
   * @param query The query.
   * @return The result, or an error.
   */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    return client.bars(std::move(query));
  }

  /**
   * @brief Returns static reference data for one instrument.
   * @param client The client.
   * @param subject The instrument.
   * @return The result, or an error.
   */
  static Result<Instrument> instrument(Client& client, InstrumentId subject) {
    return client.instrument(std::move(subject));
  }

  /**
   * @brief Returns the one account the bound credential drives.
   * @param client The client.
   * @return The result, or an error.
   */
  static Result<std::vector<AccountListItem>> accounts(Client& client) {
    return client.accounts();
  }

  /**
   * @brief Serves the normalized holdings verb once a scope is asserted; see the asynchronous thunk.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account& account) {
    if (client.config().holdings_position_scope_ == kiwoom::KiwoomPositionScope::UNSPECIFIED) {
      static_cast<void>(account);
      return refuse_holdings();
    }
    return client.holdings(account);
  }

  /**
   * @brief Returns what the bound account may spend in `currency`.
   * @param client The client.
   * @param account The bound account.
   * @param currency The currency (only KRW is answered).
   * @return The result, or an error.
   */
  static Result<BuyingPower> buying_power(
    Client& client,
    const Account& account,
    Currency currency
  ) {
    return client.buying_power(currency, account);
  }

  /**
   * @brief Submits an order for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param order The order.
   * @return The result, or an error.
   */
  static Result<PlaceAck> place_order(Client& client, const Account& account, OrderRequest order) {
    return client.place_order(account, std::move(order));
  }

  /**
   * @brief Modifies an order for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @return The result, or an error.
   */
  static Result<PlaceAck> replace_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.replace_order(account, std::move(amendment));
  }

  /**
   * @brief Cancels an order for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @return The result, or an error.
   */
  static Result<PlaceAck> cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.cancel_order(account, std::move(amendment));
  }

  /**
   * @brief Returns the amendable orders for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.open_orders(account);
  }

  /**
   * @brief Returns order history for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param query The query.
   * @return The result, or an error.
   */
  static Result<OrderPage> order_history(
    Client& client,
    const Account& account,
    HistoryQuery query
  ) {
    return client.order_history(account, std::move(query));
  }
};

} // namespace algotradelib
