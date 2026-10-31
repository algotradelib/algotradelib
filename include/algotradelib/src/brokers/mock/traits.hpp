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
#include <string_view>

namespace algotradelib {

/**
 * @brief The mock broker's adaptation: what it implements, and the thunks to `mock::MockClient`.
 *
 * It lists working orders (open orders only), reports buying power and carries the sellable quantity
 * on its holdings rows. It refuses before calling the client: a trigger price, an idempotency key, a
 * modification (`replace_unsupported`), a partial cancellation (`partial_cancel_unsupported`), and a
 * history filtered by symbol or limited to a row count.
 */
template <>
struct BrokerTraits<mock::MockClient> {
  /** @brief The client. */
  using Client = mock::MockClient;
  /** @brief The configuration: the market's settings. */
  using Config = mock::MarketSettings;
  /** @brief The account; the mock has one. */
  using Account = mock::MockAccount;
  /** @brief The broker tag. */
  using BrokerTag = mock::Broker;
  /** @brief The acknowledgement, already normalized. */
  using PlaceAck = PlacedOrder<mock::Broker>;
  /** @brief A cancellation. */
  using Amendment = mock::MockAmendment;
  /** @brief The history query, the library's own. */
  using HistoryQuery = OrderHistoryQuery;
  /** @brief The working-order listing, already normalized. */
  using WorkingOrdersResult = WorkingOrders<mock::Broker>;

  /** @brief The broker's name, `"mock"`. */
  static constexpr std::string_view name = mock::Broker::name;
  /** @brief The client returns each result. */
  static constexpr bool blocking = true;
  /** @brief It takes orders. */
  static constexpr bool data_only = false;

  /** @brief No account listing: there is one account. */
  static constexpr bool has_account_discovery = false;
  /** @brief No idempotency key. */
  static constexpr bool has_client_order_id = false;
  /** @brief Lists open orders. */
  static constexpr bool has_working_orders = true;
  /** @brief Reports each open order's quantity as amendable. */
  static constexpr bool has_amendable_quantity = true;
  /** @brief The listing shows open orders only, as every shipped broker's does. */
  static constexpr bool working_orders_state_terminal = false;
  /** @brief No stop-limit orders. */
  static constexpr bool has_trigger_price = false;
  /** @brief No single-order inquiry. */
  static constexpr bool has_order_by_id = false;
  /** @brief Cancels the whole remainder only. */
  static constexpr bool has_partial_cancel = false;
  /** @brief The history needs no date range. */
  static constexpr bool history_requires_range = false;
  /** @brief The whole history comes in one answer. */
  static constexpr bool has_history_cursor = false;
  /** @brief No row limit. */
  static constexpr bool has_history_limit = false;
  /** @brief No symbol filter. */
  static constexpr bool has_history_symbol_filter = false;
  /** @brief No order book. */
  static constexpr bool has_depth = false;
  /** @brief No price band. */
  static constexpr bool has_price_limits = false;
  /** @brief No trade prints. */
  static constexpr bool has_trade_prints = false;
  /** @brief One quote per request. */
  static constexpr bool has_batch_prices = false;
  /** @brief Reports buying power. */
  static constexpr bool has_buying_power = true;
  /** @brief No affordable quantity. */
  static constexpr bool has_buying_power_quantity = false;
  /** @brief The holdings rows carry the sellable quantity. */
  static constexpr SellableSource sellable_source = SellableSource::HOLDINGS;
  /** @brief No commission schedule; costs come from the market's fill model. */
  static constexpr bool has_commission_schedule = false;
  /** @brief Answers reference data: what the market was given, or its default. */
  static constexpr bool has_reference_data = true;
  /** @brief No reference data. */
  static constexpr bool has_batch_reference_data = false;
  /** @brief No calendar. */
  static constexpr bool has_calendar = false;
  /** @brief No calendar. */
  static constexpr bool has_calendar_range = false;
  /** @brief No calendar. */
  static constexpr bool has_calendar_sessions = false;

  /**
   * @brief Tests whether the mock can hold bars of an interval: minute to daily.
   *
   * A market answers only the interval of each instrument's own series (`unsupported_interval`).
   *
   * @param interval The interval.
   * @return `true` for every interval from a minute to a day.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    return bar_interval_length(interval).has_value() || interval == BarInterval::ONE_DAY;
  }

  /**
   * @brief Refuses a bar request for an interval no market can hold, before the client.
   * @param query The query.
   * @return `INVALID_ARGUMENT` `unsupported_interval`, or nothing.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    return refusal("unsupported_interval", "the mock broker holds minute to daily bars only");
  }

  /**
   * @brief Refuses an idempotency key, before the client.
   * @param order The order.
   * @return `INVALID_ARGUMENT` `idempotency_key_unsupported`, or nothing.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    if (!order.client_order_id_) {
      return std::nullopt;
    }
    return refusal("idempotency_key_unsupported", "the mock broker takes no client order id");
  }

  /**
   * @brief Returns the acknowledgement as it is: the market answers in the library's own shape.
   * @param raw The client's answer.
   * @return The same.
   */
  static Result<PlacedOrder<mock::Broker>> normalize_place(Result<PlaceAck> raw) { return raw; }

  /**
   * @brief Returns the listing as it is.
   * @param raw The client's answer.
   * @return The same.
   */
  static Result<WorkingOrders<mock::Broker>> normalize_working_orders(Result<WorkingOrdersResult> raw) {
    return raw;
  }

  /**
   * @brief Refuses every modification: the mock broker cancels but does not modify orders.
   * @return `INVALID_ARGUMENT` `replace_unsupported`, not sent.
   */
  static Result<Amendment> to_replacement(const OrderRef<mock::Broker>&, const AmendInstruction&) {
    return Result<Amendment>(refusal("replace_unsupported", "the mock broker does not modify orders; cancel and place"));
  }

  /**
   * @brief Converts a cancellation of the whole remainder; refuses a stated quantity.
   * @param ref The order.
   * @param instruction The cancellation.
   * @return The amendment, or `INVALID_ARGUMENT` `partial_cancel_unsupported`, not sent.
   */
  static Result<Amendment> to_cancellation(const OrderRef<mock::Broker>& ref, const AmendInstruction& instruction) {
    if (!instruction.all_remaining_) {
      return Result<Amendment>(refusal("partial_cancel_unsupported", "the mock broker cancels the whole remainder only"));
    }
    return Result<Amendment>(Amendment{ref.primary_});
  }

  /**
   * @brief Passes a history query through; refuses a symbol filter or a row limit.
   * @param query The query.
   * @return The query, or `INVALID_ARGUMENT` `history_filter_unsupported`, not sent.
   */
  static Result<HistoryQuery> to_history_query(const Account&, const OrderHistoryQuery& query) {
    if (query.symbol_ || query.limit_) {
      return Result<HistoryQuery>(
        refusal("history_filter_unsupported", "the mock broker's history takes no symbol filter or row limit")
      );
    }
    return Result<HistoryQuery>(query);
  }

  /**
   * @brief The mock broker is never paced.
   * @return `nullptr`.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client&) { return nullptr; }

  /**
   * @brief Forwards a quote request.
   * @param client The client.
   * @param instrument The instrument.
   * @return The client's answer.
   */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) { return client.quote(instrument); }

  /**
   * @brief Forwards a reference-data request.
   * @param client The client.
   * @param instrument The instrument.
   * @return The client's answer.
   */
  static Result<Instrument> instrument(Client& client, InstrumentId instrument) { return client.instrument(instrument); }

  /**
   * @brief Forwards a bar request.
   * @param client The client.
   * @param query The query.
   * @return The client's answer.
   */
  static Result<BarSeries> bars(Client& client, BarQuery query) { return client.bars(query); }

  /**
   * @brief Forwards a holdings request.
   * @param client The client.
   * @return The client's answer.
   */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account&) { return client.holdings(); }

  /**
   * @brief Forwards a buying-power request.
   * @param client The client.
   * @param currency The currency.
   * @return The client's answer.
   */
  static Result<BuyingPower> buying_power(Client& client, const Account&, Currency currency) {
    return client.buying_power(currency);
  }

  /**
   * @brief Forwards a placement.
   * @param client The client.
   * @param order The order.
   * @return The client's answer.
   */
  static Result<PlaceAck> place_order(Client& client, const Account&, OrderRequest order) {
    return client.place(order);
  }

  /**
   * @brief Never called: `to_replacement` refuses every modification.
   * @return A refusal.
   */
  static Result<PlaceAck> replace_order(Client&, const Account&, Amendment) {
    return Result<PlaceAck>(refusal("replace_unsupported", "the mock broker does not modify orders; cancel and place"));
  }

  /**
   * @brief Forwards a cancellation.
   * @param client The client.
   * @param amendment The order id.
   * @return The client's answer.
   */
  static Result<PlaceAck> cancel_order(Client& client, const Account&, Amendment amendment) {
    return client.cancel(amendment);
  }

  /**
   * @brief Forwards a working-order request.
   * @param client The client.
   * @return The client's answer.
   */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account&) { return client.working_orders(); }

  /**
   * @brief Forwards an order-history request.
   * @param client The client.
   * @param query The query.
   * @return The client's answer.
   */
  static Result<OrderPage> order_history(Client& client, const Account&, HistoryQuery query) {
    return client.order_history(query);
  }

private:
  /** Returns a refusal made before anything was sent. */
  static BrokerError refusal(const char* code, const char* message) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = code;
    error.message_ = message;
    return error;
  }
};

} // namespace algotradelib
