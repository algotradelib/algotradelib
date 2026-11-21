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

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {

/**
 * @brief The `BrokerSession` adapter for `alpaca::AlpacaBlockingClient`.
 *
 * A US listing is `InstrumentId{Country::US, Venue::UNSPECIFIED, symbol}`, in dollars.
 * Every placement is checked by `alpaca::detail::validate_order` (this traits' `validate_place` and the client's own
 * `place_order` alike), then by the client's reads of the asset, the position and the clock.
 * Replacements are refused: cancel and place instead, as a runner does.
 * It declares no `no_account()`: the keys are the account, so every `AlpacaAccount` trades.
 */
template <>
struct BrokerTraits<alpaca::AlpacaBlockingClient> {
  /** @brief The client type. */
  using Client = alpaca::AlpacaBlockingClient;

  /** @brief The keys and environment. */
  using Config = alpaca::AlpacaConfig;

  /** @brief The account selector, which carries nothing. */
  using Account = alpaca::AlpacaAccount;

  /** @brief Item type of the broker's own account listing. */
  using AccountListItem = alpaca::AlpacaAccountInfo;

  /** @brief The broker tag. */
  using BrokerTag = alpaca::Broker;

  /** @brief Alpaca's acknowledgement of a placement or cancellation. */
  using PlaceAck = alpaca::AlpacaOrderAck;

  /** @brief A cancellation of one whole order. */
  using Amendment = alpaca::AlpacaAmendment;

  /** @brief The history query, the library's own. */
  using HistoryQuery = OrderHistoryQuery;

  /** @brief The open orders, before normalizing. */
  using WorkingOrdersResult = alpaca::AlpacaOpenOrders;

  /** @brief The broker's name. */
  static constexpr std::string_view name = alpaca::Broker::name;

  /** @brief `true`: requests return their result directly. */
  static constexpr bool blocking = true;

  /** @brief A broker that takes orders, not a market-data source. */
  static constexpr bool data_only = false;

  /** @brief `true`: `/v2/account` reports the one account the keys drive. */
  static constexpr bool has_account_discovery = true;

  /** @brief `true`: `client_order_id`, 1 to 128 characters, unique for the account. */
  static constexpr bool has_client_order_id = true;

  /** @brief `true`: `/v2/orders?status=open`. */
  static constexpr bool has_working_orders = true;

  /** @brief `true`: each order's `qty` less its `filled_qty`. */
  static constexpr bool has_amendable_quantity = true;

  /** @brief `false`: the open listing shows open orders only. */
  static constexpr bool working_orders_state_terminal = false;

  /** @brief `false`: this adapter sends no stop orders. */
  static constexpr bool has_trigger_price = false;

  /** @brief `true`: `/v2/orders/{id}`. */
  static constexpr bool has_order_by_id = true;

  /** @brief `false`: Alpaca cancels the whole order. */
  static constexpr bool has_partial_cancel = false;

  /** @brief `false`: a range is optional. */
  static constexpr bool history_requires_range = false;

  /** @brief `true`: a full page carries its last row's submission time, sent back as Alpaca's `until`. */
  static constexpr bool has_history_cursor = true;

  /** @brief `true`: up to `history_limit` rows a page. */
  static constexpr bool has_history_limit = true;

  /** @brief The most rows one history page may ask for. */
  static constexpr int history_limit = 500;

  /** @brief `true`: Alpaca filters by `symbols=`. */
  static constexpr bool has_history_symbol_filter = true;

  /** @brief `false`: served by Alpaca, not adapted. */
  static constexpr bool has_depth = false;

  /** @brief `false`: no daily price band is published for US equities. */
  static constexpr bool has_price_limits = false;

  /** @brief `false`: served by Alpaca, not adapted. */
  static constexpr bool has_trade_prints = false;

  /** @brief `false`: one instrument a request. */
  static constexpr bool has_batch_prices = false;

  /**
   * @brief `false`: no Alpaca figure is cash net of resting buys without credit (`buying_power` counts margin), so
   *   buys are not checked against cash; a buy above the cash borrows on the account's margin. `runner::PollingLoop`
   *   over it holds every placement unless `RiskLimits::run_budget_` is set, which a runner that sends to a broker
   *   does not take yet.
   */
  static constexpr bool has_buying_power = false;

  /** @brief `false`: there is no buying power to express in shares. */
  static constexpr bool has_buying_power_quantity = false;

  /** @brief `HOLDINGS`: each position's `qty_available`, with the basis `AlpacaBlockingClient::holdings` states. */
  static constexpr SellableSource sellable_source = SellableSource::HOLDINGS;

  /** @brief `false`: commission-free, and the per-share FINRA fee on sells has no `CommissionRate` shape. */
  static constexpr bool has_commission_schedule = false;

  /** @brief `true`: `/v2/assets/{symbol}`. */
  static constexpr bool has_reference_data = true;

  /** @brief `false`: one instrument a request. */
  static constexpr bool has_batch_reference_data = false;

  /** @brief `true`: `/v3/calendar/XNYS`. */
  static constexpr bool has_calendar = true;

  /** @brief `true`: one request covers the whole range. */
  static constexpr bool has_calendar_range = true;

  /**
   * @brief `true`: the adapter fills each open day's regular session and, where stated, its extended span, so a live
   *   loop takes the market phase from them.
   */
  static constexpr bool has_calendar_sessions = true;

  /**
   * @brief Tests whether this adapter serves bars at an interval.
   * @param interval The interval.
   * @return `true` for 1, 5, 15 and 30 minutes, an hour and a day.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    return interval == BarInterval::ONE_MINUTE || interval == BarInterval::FIVE_MINUTES ||
      interval == BarInterval::FIFTEEN_MINUTES || interval == BarInterval::THIRTY_MINUTES ||
      interval == BarInterval::ONE_HOUR || interval == BarInterval::ONE_DAY;
  }

  /**
   * @brief Returns where an order goes: Alpaca routes it, and no venue can be asked for.
   * @return `Venue::UNSPECIFIED`.
   */
  static constexpr Venue effective_route(Venue) noexcept {
    return Venue::UNSPECIFIED;
  }

  /**
   * @brief Refuses a bar request this adapter cannot answer as asked, before any I/O.
   * @param query The query.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return alpaca::detail::validate_bar_query(query);
  }

  /**
   * @brief Refuses a calendar for any market but the US, or with an invalid date.
   * @param country The market.
   * @param from The first date.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_calendar(Country country, std::optional<Date> from) {
    if (country != Country::US) {
      return alpaca::detail::refusal("market_unsupported", "Alpaca's calendar is the US market's");
    }
    if (from && !is_valid_date(*from)) {
      return alpaca::detail::refusal("invalid_date", "the calendar needs a valid date");
    }
    return std::nullopt;
  }

  /**
   * @brief Refuses an order this adapter does not send as described, before any I/O; the same check the client's
   *   `place_order` makes first.
   * @param order The order.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    return alpaca::detail::validate_order(order);
  }

  /**
   * @brief Normalizes Alpaca's acknowledgement; the reference is the order id.
   * @param raw The client's result.
   * @return The placed order, or the error.
   */
  static Result<PlacedOrder<alpaca::Broker>> normalize_place(Result<PlaceAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<alpaca::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<alpaca::Broker> placed;
    placed.ack_.order_id_ = raw.value().order_id_;
    placed.ack_.client_order_id_ = raw.value().client_order_id_;
    placed.ack_.accepted_at_ = raw.value().received_at_;
    placed.ref_.primary_ = raw.value().order_id_;
    return Result<PlacedOrder<alpaca::Broker>>(std::move(placed));
  }

  /**
   * @brief Converts the open orders into working orders, each amendable for its unfilled quantity.
   * @param raw The client's result.
   * @return The working orders, or the error.
   */
  static Result<WorkingOrders<alpaca::Broker>> normalize_working_orders(Result<WorkingOrdersResult> raw) {
    if (!raw) {
      return Result<WorkingOrders<alpaca::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<alpaca::Broker> orders;
    orders.received_at_ = raw.value().received_at_;
    for (auto& open : raw.value().orders_) {
      WorkingOrder<alpaca::Broker> working;
      working.ref_.primary_ = open.order_id_;
      working.amendable_quantity_ = decimal_subtract(open.quantity_, open.execution_.filled_quantity_);
      working.order_ = std::move(open);
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<alpaca::Broker>>(std::move(orders));
  }

  /**
   * @brief Refuses every replacement: Alpaca's replacement is a new order that drops the `client_order_id`, and a
   *   runner never replaces (it cancels and places).
   * @return An `INVALID_ARGUMENT` error (`replace_unsupported`, not sent).
   */
  static Result<Amendment> to_replacement(const OrderRef<alpaca::Broker>&, const AmendInstruction&) {
    return Result<Amendment>(
      alpaca::detail::refusal("replace_unsupported", "the Alpaca adapter does not replace orders; cancel and place")
    );
  }

  /**
   * @brief Converts a cancellation of the whole order; a stated quantity is refused.
   * @param ref The order.
   * @param instruction The cancellation.
   * @return The amendment, or an `INVALID_ARGUMENT` error.
   */
  static Result<Amendment> to_cancellation(const OrderRef<alpaca::Broker>& ref, const AmendInstruction& instruction) {
    if (!instruction.all_remaining_) {
      return Result<Amendment>(
        alpaca::detail::refusal("partial_cancel_unsupported", "Alpaca cancels the whole order only")
      );
    }
    if (!alpaca::detail::valid_order_id(ref.primary_)) {
      return Result<Amendment>(alpaca::detail::invalid_order_id());
    }
    return Result<Amendment>(Amendment{ref.primary_});
  }

  /**
   * @brief Admits a history query with a limit of 1 to 500 and valid dates.
   * @param query The query.
   * @return The query, or an `INVALID_ARGUMENT` error.
   */
  static Result<HistoryQuery> to_history_query(const Account&, const OrderHistoryQuery& query) {
    if (query.limit_ && (*query.limit_ <= 0 || *query.limit_ > history_limit)) {
      return Result<HistoryQuery>(alpaca::detail::refusal("invalid_limit", "Alpaca's order list takes 1 to 500 rows"));
    }
    if ((query.from_ && !is_valid_date(*query.from_)) || (query.to_ && !is_valid_date(*query.to_))) {
      return Result<HistoryQuery>(alpaca::detail::refusal("invalid_date", "the history range needs valid dates"));
    }
    return Result<HistoryQuery>(query);
  }

  /**
   * @brief Returns the account selector a session binds, which is empty. The argument is not used.
   * @return An empty selector.
   */
  static Account selector_of(const AccountListItem&) {
    return Account{};
  }

  /**
   * @brief Returns the account number as Alpaca reports it.
   * @param account The account.
   * @return The number.
   */
  static std::string display_of(const AccountListItem& account) {
    return account.account_number_;
  }

  /**
   * @brief Returns the account type, which this adapter does not read (the argument is not used).
   * @return An empty string.
   */
  static std::string type_of(const AccountListItem&) {
    return {};
  }

  /**
   * @brief Returns the trading connection's rate-limit statistics.
   * @param client The client.
   * @return The statistics, or null if the connection has no budget.
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
   * @brief Returns bars.
   * @param client The client.
   * @param query The query.
   * @return The result, or an error.
   */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    return client.bars(std::move(query));
  }

  /**
   * @brief Returns reference data for one instrument.
   * @param client The client.
   * @param subject The instrument.
   * @return The result, or an error.
   */
  static Result<Instrument> instrument(Client& client, InstrumentId subject) {
    return client.instrument(std::move(subject));
  }

  /**
   * @brief Returns the trading calendar.
   * @param client The client.
   * @param country The market.
   * @param from The first date, or empty for the default range.
   * @return The result, or an error.
   */
  static Result<TradingCalendar> calendar(Client& client, Country country, std::optional<Date> from) {
    return client.calendar(country, from);
  }

  /**
   * @brief Returns the one account the keys drive.
   * @param client The client.
   * @return The result, or an error.
   */
  static Result<std::vector<AccountListItem>> accounts(Client& client) {
    return client.accounts();
  }

  /**
   * @brief Returns the holdings.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account& account) {
    return client.holdings(account);
  }

  /**
   * @brief Places an order.
   * @param client The client.
   * @param account The bound account.
   * @param order The order.
   * @return The result, or an error.
   */
  static Result<PlaceAck> place_order(Client& client, const Account& account, OrderRequest order) {
    return client.place_order(account, std::move(order));
  }

  /**
   * @brief Refuses a replacement; `to_replacement` refuses first, so a session never calls it.
   * @return An `INVALID_ARGUMENT` error (`replace_unsupported`, not sent).
   */
  static Result<PlaceAck> replace_order(Client&, const Account&, Amendment) {
    return Result<PlaceAck>(
      alpaca::detail::refusal("replace_unsupported", "the Alpaca adapter does not replace orders; cancel and place")
    );
  }

  /**
   * @brief Cancels an order.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The order.
   * @return The result, or an error.
   */
  static Result<PlaceAck> cancel_order(Client& client, const Account& account, Amendment amendment) {
    return client.cancel_order(account, std::move(amendment));
  }

  /**
   * @brief Returns the open orders.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.working_orders(account);
  }

  /**
   * @brief Returns one order by its id.
   * @param client The client.
   * @param account The bound account.
   * @param id The order id.
   * @return The result, or an error.
   */
  static Result<OrderSnapshot> order_by_id(Client& client, const Account& account, const std::string& id) {
    return client.order_by_id(account, id);
  }

  /**
   * @brief Returns a page of order history.
   * @param client The client.
   * @param account The bound account.
   * @param query The admitted query.
   * @return The result, or an error.
   */
  static Result<OrderPage> order_history(Client& client, const Account& account, HistoryQuery query) {
    return client.order_history(account, std::move(query));
  }
};

} // namespace algotradelib
