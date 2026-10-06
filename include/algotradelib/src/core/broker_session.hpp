#pragma once

#include "./broker_caps.hpp"
#include "./broker_traits.hpp"

#include <algotradelib/network>

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/system_executor.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {

namespace port_detail {

// Converts a broker's account listing into `AccountInfo` rows (shared by both sessions).
template <typename Client, typename Items>
Result<std::vector<AccountInfo<Client>>> to_account_info(Result<Items> raw) {
  using Traits = BrokerTraits<Client>;
  using Accounts = std::vector<AccountInfo<Client>>;
  if (!raw) {
    return Result<Accounts>(std::move(raw.error()));
  }
  Accounts listed;
  listed.reserve(raw.value().size());
  for (const auto& item : raw.value()) {
    AccountInfo<Client> info;
    info.display_number_ = Traits::display_of(item);
    info.type_ = Traits::type_of(item);
    info.selector_ = Traits::selector_of(item);
    listed.push_back(std::move(info));
  }
  return Result<Accounts>(std::move(listed));
}

/**
 * Refuses a trigger price bound for a broker that does not declare it transmits one.
 *
 * Checked here, before the adapter, because this is the one layer every adapter passes through --
 * including one written before `OrderRequest::trigger_price_` existed, which would otherwise send
 * a stop order without its releasing condition or a limit order with its trigger silently gone.
 * Whether *this basis* wants a trigger is the adapter's question and is not asked here.
 *
 * Applied to placements and modifications only. A cancellation withdraws an order, and
 * `cancel_all` restates whatever trigger a listing reported, so refusing one here could make a
 * stop order impossible to withdraw -- which is the failure this library must never manufacture.
 */
template <typename Client>
std::optional<BrokerError> trigger_refusal(const std::optional<Decimal64>& trigger) {
  if constexpr (BrokerCaps<Client>::has_trigger_price) {
    return std::nullopt;
  } else {
    if (!trigger) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "unsupported_trigger_price";
    error.message_ =
      "this broker does not declare that it transmits a trigger price, so an order carrying one "
      "would be sent without it; place the order without a trigger or through a broker whose "
      "BrokerCaps::has_trigger_price is true";
    return error;
  }
}

/**
 * A placement's pre-flight, shared by both sessions: the trigger rule, then the adapter's own validation.
 *
 * The order matters and is written here once. A trigger the broker cannot send is refused before the
 * adapter is asked anything, so an adapter that has never read `trigger_price_` cannot pass the order on
 * without it.
 */
template <typename Client>
std::optional<BrokerError> prepare_place(const OrderRequest& order) {
  if (auto refused = trigger_refusal<Client>(order.trigger_price_)) {
    return refused;
  }
  return BrokerTraits<Client>::validate_place(order);
}

/**
 * A modification's pre-flight, shared by both sessions: the trigger rule on the amended order, then the
 * adapter's conversion.
 *
 * `amended_trigger_price` is the trigger the amended order would carry, whether the instruction states it
 * or keeps the resting one. **A cancellation has no counterpart here, deliberately**: it calls
 * `to_cancellation` directly, because refusing a trigger there could leave a stop order that cannot be
 * withdrawn (see `trigger_refusal`).
 */
template <typename Client>
Result<typename BrokerTraits<Client>::Amendment> prepare_replace(
  const OrderRef<typename BrokerTraits<Client>::BrokerTag>& ref, const AmendInstruction& instruction
) {
  using Amendment = typename BrokerTraits<Client>::Amendment;
  if (auto refused = trigger_refusal<Client>(amended_trigger_price(instruction))) {
    return Result<Amendment>(std::move(*refused));
  }
  return BrokerTraits<Client>::to_replacement(ref, instruction);
}

} // namespace port_detail

/**
 * @brief An asynchronous broker client bound to one account, with broker-neutral operations.
 *
 * Every operation takes a completion token and completes once with `Result<T>`, on the handler's
 * associated executor. An operation the broker does not offer (see `Caps`) does not compile; gate
 * it with `if constexpr`. A request the session or adapter refuses before any I/O completes with an
 * error that is posted, never invoked inline. A session is cheap to copy (a shared client pointer
 * and the account).
 *
 * @tparam Client An asynchronous broker client (`BrokerTraits<Client>::blocking` is `false`); use
 *   `BlockingSession` for a blocking one.
 */
template <typename Client>
class BrokerSession {
public:
  /** @brief The client's traits. */
  using Traits = BrokerTraits<Client>;

  /** @brief The client's capabilities. */
  using Caps = BrokerCaps<Client>;

  /** @brief The broker's account selector type. */
  using Account = typename Traits::Account;

  /** @brief The broker's configuration type. */
  using Config = typename Traits::Config;

  /** @brief The broker's tag type. */
  using BrokerTag = typename Traits::BrokerTag;

  /** @brief The order reference type this session issues and accepts. */
  using Ref = OrderRef<BrokerTag>;

  /** @brief What an order-writing call returns: acknowledgement and reference. */
  using Placed = PlacedOrder<BrokerTag>;

  /** @brief One working order of this broker. */
  using Working = WorkingOrder<BrokerTag>;

  /** @brief A working-order listing of this broker. */
  using WorkingSet = WorkingOrders<BrokerTag>;

  static_assert(
    !Traits::blocking,
    "BrokerSession requires an asynchronous client; use BlockingSession for a blocking facade"
  );

  /**
   * @brief Binds a client and an account.
   * @param client The client; must not be null.
   * @param account The account to act for.
   * @throws std::invalid_argument if `client` is null.
   */
  BrokerSession(std::shared_ptr<Client> client, Account account)
  : client_(std::move(client)), account_(std::move(account)) {
    if (!client_) {
      throw std::invalid_argument("broker session requires a client");
    }
  }

  /**
   * @brief Returns the broker's name.
   * @return `Traits::name`.
   */
  static constexpr std::string_view broker_name() noexcept { return Traits::name; }

  /**
   * @brief Returns the bound account.
   * @return The account selector.
   */
  const Account& account() const noexcept { return account_; }

  /**
   * @brief Returns the broker client, for broker-specific operations the session does not offer.
   *
   * Use it inside an `if constexpr` on `Caps` so the broker-specific code compiles only where the
   * feature exists.
   *
   * @return The client.
   */
  Client& client() noexcept { return *client_; }

  /**
   * @brief Returns the broker client.
   * @return The client.
   */
  const Client& client() const noexcept { return *client_; }

  /**
   * @brief Returns the shared client pointer, for binding a second session.
   * @return The client pointer.
   */
  const std::shared_ptr<Client>& shared_client() const noexcept { return client_; }

  /**
   * @brief Returns the client's request-pacing counters.
   *
   * A rising `delayed_` on a polling group while `TRADING` stays undelayed means trading capacity
   * is being kept free; a rising `shed_` means the queue is full. Read them from a completion
   * handler, since they live on the client's strand.
   *
   * @return The counters, or `nullptr` if the client is not paced.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    return Traits::rate_limit_stats(*client_);
  }

  /** @brief Cancels the client's transport work and queued operations. */
  void close() { client_->close(); }

  /**
   * @brief Requests the current quote for an instrument.
   *
   * @tparam CompletionToken A completion token for `void(Result<QuoteSnapshot>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instrument The instrument.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_quote(InstrumentId instrument, CompletionToken&& token) {
    return translate<QuoteSnapshot>(
      std::forward<CompletionToken>(token),
      [client = client_, instrument = std::move(instrument)](auto&& sink) mutable {
        Traits::quote(*client, std::move(instrument), std::forward<decltype(sink)>(sink));
      },
      [](Result<QuoteSnapshot> raw) { return raw; }
    );
  }

  /**
   * @brief Requests the account's holdings.
   *
   * @tparam CompletionToken A completion token for `void(Result<HoldingsSnapshot>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_holdings(CompletionToken&& token) {
    return translate<HoldingsSnapshot>(
      std::forward<CompletionToken>(token),
      [client = client_, account = account_](auto&& sink) mutable {
        Traits::holdings(*client, account, std::forward<decltype(sink)>(sink));
      },
      [](Result<HoldingsSnapshot> raw) { return raw; }
    );
  }

  /**
   * @brief Requests what the account may spend in a currency.
   *
   * The figure was true when the broker answered; a fill since can change it. Requires
   * `Caps::has_buying_power`.
   *
   * @tparam CompletionToken A completion token for `void(Result<BuyingPower>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param currency The currency; one the broker does not price is refused with
   *   `INVALID_ARGUMENT` before any I/O.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_buying_power(Currency currency, CompletionToken&& token) {
    if constexpr (Caps::has_buying_power) {
      return translate<BuyingPower>(
        std::forward<CompletionToken>(token),
        [client = client_, account = account_, currency](auto&& sink) mutable {
          Traits::buying_power(*client, account, currency, std::forward<decltype(sink)>(sink));
        },
        [](Result<BuyingPower> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not answer what the account may spend; gate the call with "
        "BrokerCaps<Client>::has_buying_power"
      );
    }
  }

  /**
   * @brief Requests how much of an instrument the account may sell.
   *
   * Only where `Caps::sellable_source` is `INQUIRY`; elsewhere read
   * `HoldingPosition::sellable_quantity_` from the holdings.
   *
   * @tparam CompletionToken A completion token for `void(Result<Decimal64>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instrument The instrument.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_sellable_quantity(InstrumentId instrument, CompletionToken&& token) {
    if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
      return translate<Decimal64>(
        std::forward<CompletionToken>(token),
        [client = client_, account = account_, instrument = std::move(instrument)](
          auto&& sink
        ) mutable {
          Traits::sellable_quantity(
            *client, account, std::move(instrument), std::forward<decltype(sink)>(sink)
          );
        },
        [](Result<Decimal64> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker has no separate sellable-quantity inquiry; read "
        "HoldingPosition::sellable_quantity_ from the holdings snapshot, and gate on "
        "BrokerCaps<Client>::sellable_source"
      );
    }
  }

  /**
   * @brief Requests the account's commission rates.
   *
   * Requires `Caps::has_commission_schedule`. An empty schedule means the account has no stated
   * rates.
   *
   * @tparam CompletionToken A completion token for `void(Result<CommissionSchedule>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_commissions(CompletionToken&& token) {
    if constexpr (Caps::has_commission_schedule) {
      return translate<CommissionSchedule>(
        std::forward<CompletionToken>(token),
        [client = client_, account = account_](auto&& sink) mutable {
          Traits::commissions(*client, account, std::forward<decltype(sink)>(sink));
        },
        [](Result<CommissionSchedule> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker publishes no commission schedule; gate the call with "
        "BrokerCaps<Client>::has_commission_schedule"
      );
    }
  }

  /**
   * @brief Places a new order.
   *
   * @warning A `TRANSPORT` error leaves the outcome unknown: the request may have reached the broker.
   *   The request is never resent automatically. Check working orders or order history before
   *   sending it again.
   *
   * @tparam CompletionToken A completion token for `void(Result<Placed>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param order The order. An idempotency key at a broker without one, a trigger at a broker
   *   without `Caps::has_trigger_price`, or a combination the adapter does not offer is refused
   *   before any I/O.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_place(OrderRequest order, CompletionToken&& token) {
    if (auto refused = port_detail::prepare_place<Client>(order)) {
      return complete_error<Placed>(
        std::forward<CompletionToken>(token), std::move(*refused)
      );
    }
    return translate<Placed>(
      std::forward<CompletionToken>(token),
      [client = client_, account = account_, order = std::move(order)](auto&& sink) mutable {
        Traits::place_order(
          *client, account, std::move(order), std::forward<decltype(sink)>(sink)
        );
      },
      [](Result<typename Traits::PlaceAck> raw) { return Traits::normalize_place(std::move(raw)); }
    );
  }

  /**
   * @brief Modifies a working order.
   *
   * @warning A `TRANSPORT` error leaves the outcome unknown: the request may have reached the broker.
   *   The request is never resent automatically. Check working orders or order history before
   *   sending it again.
   *
   * @tparam CompletionToken A completion token for `void(Result<Placed>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param ref The order.
   * @param instruction What to change; build it with `replace_with` or `reprice`.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_replace(Ref ref, AmendInstruction instruction, CompletionToken&& token) {
    auto amendment = port_detail::prepare_replace<Client>(ref, instruction);
    if (!amendment) {
      return complete_error<Placed>(
        std::forward<CompletionToken>(token), std::move(amendment.error())
      );
    }
    return translate<Placed>(
      std::forward<CompletionToken>(token),
      [
        client = client_,
        account = account_,
        value = std::move(amendment.value())
      ](auto&& sink) mutable {
        Traits::replace_order(
          *client, account, std::move(value), std::forward<decltype(sink)>(sink)
        );
      },
      [](Result<typename Traits::PlaceAck> raw) { return Traits::normalize_place(std::move(raw)); }
    );
  }

  /**
   * @brief Cancels a working order, in full or in part.
   *
   * @warning A `TRANSPORT` error leaves the outcome unknown: the request may have reached the broker.
   *   The request is never resent automatically. Check working orders or order history before
   *   sending it again.
   *
   * @tparam CompletionToken A completion token for `void(Result<Placed>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param ref The order. A reference from another broker does not compile.
   * @param instruction Build it with `cancel_all` or `cancel_quantity`; a partial cancellation at
   *   a broker that cancels only in full is refused before any I/O. A trigger price in it is never
   *   refused, so a stop order can always be withdrawn.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_cancel(Ref ref, AmendInstruction instruction, CompletionToken&& token) {
    auto amendment = Traits::to_cancellation(ref, instruction);
    if (!amendment) {
      return complete_error<Placed>(
        std::forward<CompletionToken>(token), std::move(amendment.error())
      );
    }
    return translate<Placed>(
      std::forward<CompletionToken>(token),
      [
        client = client_,
        account = account_,
        value = std::move(amendment.value())
      ](auto&& sink) mutable {
        Traits::cancel_order(
          *client, account, std::move(value), std::forward<decltype(sink)>(sink)
        );
      },
      [](Result<typename Traits::PlaceAck> raw) { return Traits::normalize_place(std::move(raw)); }
    );
  }

  /**
   * @brief Requests the orders still open to modification or cancellation.
   *
   * Requires `Caps::has_working_orders`. KIS does not serve this inquiry to demo accounts; its
   * adapter answers from the day's order history there.
   *
   * @tparam CompletionToken A completion token for `void(Result<WorkingSet>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_working_orders(CompletionToken&& token) {
    if constexpr (Caps::has_working_orders) {
      return translate<WorkingSet>(
        std::forward<CompletionToken>(token),
        [client = client_, account = account_](auto&& sink) mutable {
          Traits::working_orders(*client, account, std::forward<decltype(sink)>(sink));
        },
        [](Result<typename Traits::WorkingOrdersResult> raw) {
          return Traits::normalize_working_orders(std::move(raw));
        }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish its amendable orders; gate the call with "
        "BrokerCaps<Client>::has_working_orders"
      );
    }
  }

  /**
   * @brief Requests order history: placed orders and their executions.
   *
   * @tparam CompletionToken A completion token for `void(Result<OrderPage>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param query The filter; one the broker cannot apply is refused with `INVALID_ARGUMENT` before
   *   any I/O.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_order_history(OrderHistoryQuery query, CompletionToken&& token) {
    auto converted = Traits::to_history_query(account_, query);
    if (!converted) {
      return complete_error<OrderPage>(
        std::forward<CompletionToken>(token), std::move(converted.error())
      );
    }
    return translate<OrderPage>(
      std::forward<CompletionToken>(token),
      [
        client = client_,
        account = account_,
        value = std::move(converted.value())
      ](auto&& sink) mutable {
        Traits::order_history(
          *client, account, std::move(value), std::forward<decltype(sink)>(sink)
        );
      },
      [](Result<OrderPage> raw) { return raw; }
    );
  }

  /**
   * @brief Requests bars for an instrument, paging until `count_` bars are read.
   *
   * Bars arrive oldest first, with the broker's stamp convention in `BarSeries::stamp_`.
   *
   * @tparam CompletionToken A completion token for `void(Result<BarSeries>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param query What to fetch; an interval the broker does not publish is refused before any I/O
   *   (see `Caps::supports_interval`).
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_bars(BarQuery query, CompletionToken&& token) {
    if (auto invalid = Traits::validate_bars(query)) {
      return complete_error<BarSeries>(
        std::forward<CompletionToken>(token), std::move(*invalid)
      );
    }
    return translate<BarSeries>(
      std::forward<CompletionToken>(token),
      [client = client_, query = std::move(query)](auto&& sink) mutable {
        Traits::bars(*client, std::move(query), std::forward<decltype(sink)>(sink));
      },
      [](Result<BarSeries> raw) { return raw; }
    );
  }

  /**
   * @brief Requests an instrument's reference data.
   *
   * Requires `Caps::has_reference_data`. One instrument per call; for many, see
   * `Caps::has_batch_reference_data` and `client()`. `price_rule_` is the library's own grid.
   *
   * @tparam CompletionToken A completion token for `void(Result<Instrument>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instrument The instrument.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_instrument(InstrumentId instrument, CompletionToken&& token) {
    if constexpr (Caps::has_reference_data) {
      return translate<Instrument>(
        std::forward<CompletionToken>(token),
        [client = client_, instrument = std::move(instrument)](auto&& sink) mutable {
          Traits::instrument(*client, std::move(instrument), std::forward<decltype(sink)>(sink));
        },
        [](Result<Instrument> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish instrument reference data; gate the call with "
        "BrokerCaps<Client>::has_reference_data"
      );
    }
  }

  /**
   * @brief Requests the trading calendar.
   *
   * Requires `Caps::has_calendar`. How many dates one answer covers differs by broker
   * (`Caps::has_calendar_range`).
   *
   * @tparam CompletionToken A completion token for `void(Result<TradingCalendar>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param country The market.
   * @param from The date the answer is anchored to; empty means today.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_calendar(Country country, std::optional<Date> from, CompletionToken&& token) {
    if constexpr (Caps::has_calendar) {
      if (auto invalid = Traits::validate_calendar(country, from)) {
        return complete_error<TradingCalendar>(
          std::forward<CompletionToken>(token), std::move(*invalid)
        );
      }
      return translate<TradingCalendar>(
        std::forward<CompletionToken>(token),
        [client = client_, country, from](auto&& sink) mutable {
          Traits::calendar(*client, country, from, std::forward<decltype(sink)>(sink));
        },
        [](Result<TradingCalendar> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish a trading calendar; gate the call with "
        "BrokerCaps<Client>::has_calendar"
      );
    }
  }

  /**
   * @brief Requests an instrument's price limits for today.
   *
   * Requires `Caps::has_price_limits`.
   *
   * @tparam CompletionToken A completion token for `void(Result<PriceLimits>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instrument The instrument.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_price_limits(InstrumentId instrument, CompletionToken&& token) {
    if constexpr (Caps::has_price_limits) {
      return translate<PriceLimits>(
        std::forward<CompletionToken>(token),
        [client = client_, instrument = std::move(instrument)](auto&& sink) mutable {
          Traits::price_limits(
            *client, std::move(instrument), std::forward<decltype(sink)>(sink)
          );
        },
        [](Result<PriceLimits> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish price limits; gate the call with "
        "BrokerCaps<Client>::has_price_limits"
      );
    }
  }

  /**
   * @brief Requests recent trades for an instrument, newest first.
   *
   * Requires `Caps::has_trade_prints`.
   *
   * @tparam CompletionToken A completion token for `void(Result<std::vector<TradePrint>>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instrument The instrument.
   * @param count How many trades to request.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_trades(InstrumentId instrument, int count, CompletionToken&& token) {
    if constexpr (Caps::has_trade_prints) {
      return translate<std::vector<TradePrint>>(
        std::forward<CompletionToken>(token),
        [client = client_, instrument = std::move(instrument), count](auto&& sink) mutable {
          Traits::trades(
            *client, std::move(instrument), count, std::forward<decltype(sink)>(sink)
          );
        },
        [](Result<std::vector<TradePrint>> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish recent executions; gate the call with "
        "BrokerCaps<Client>::has_trade_prints"
      );
    }
  }

  /**
   * @brief Requests quotes for several instruments in one request.
   *
   * Requires `Caps::has_batch_prices`; otherwise call `async_quote` per instrument.
   *
   * @tparam CompletionToken A completion token for `void(Result<std::vector<QuoteSnapshot>>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instruments The instruments.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_prices(std::vector<InstrumentId> instruments, CompletionToken&& token) {
    if constexpr (Caps::has_batch_prices) {
      return translate<std::vector<QuoteSnapshot>>(
        std::forward<CompletionToken>(token),
        [client = client_, instruments = std::move(instruments)](auto&& sink) mutable {
          Traits::prices(*client, std::move(instruments), std::forward<decltype(sink)>(sink));
        },
        [](Result<std::vector<QuoteSnapshot>> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker quotes one instrument per request; gate the call with "
        "BrokerCaps<Client>::has_batch_prices and call async_quote per instrument"
      );
    }
  }

  /**
   * @brief Requests an instrument's order book, best levels first.
   *
   * Requires `Caps::has_depth`.
   *
   * @tparam CompletionToken A completion token for `void(Result<DepthSnapshot>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param instrument The instrument.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_depth(InstrumentId instrument, CompletionToken&& token) {
    if constexpr (Caps::has_depth) {
      return translate<DepthSnapshot>(
        std::forward<CompletionToken>(token),
        [client = client_, instrument = std::move(instrument)](auto&& sink) mutable {
          Traits::depth(*client, std::move(instrument), std::forward<decltype(sink)>(sink));
        },
        [](Result<DepthSnapshot> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish an order book; gate the call with "
        "BrokerCaps<Client>::has_depth"
      );
    }
  }

  /**
   * @brief Requests one order by its reference.
   *
   * Requires `Caps::has_order_by_id`; otherwise read it from working orders or order history.
   *
   * @tparam CompletionToken A completion token for `void(Result<OrderSnapshot>)`, such as a callback or
   *   `boost::asio::use_future`.
   * @param ref The order.
   * @param token The completion token; the handler runs once, on its associated executor.
   * @return Whatever `token` produces (nothing for a callback, a future for `use_future`).
   */
  template <typename CompletionToken>
  auto async_order(Ref ref, CompletionToken&& token) {
    if constexpr (Caps::has_order_by_id) {
      return translate<OrderSnapshot>(
        std::forward<CompletionToken>(token),
        [client = client_, account = account_, id = ref.primary_](auto&& sink) mutable {
          Traits::order_by_id(*client, account, std::move(id), std::forward<decltype(sink)>(sink));
        },
        [](Result<OrderSnapshot> raw) { return raw; }
      );
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not retrieve one order by identifier; gate the call with "
        "BrokerCaps<Client>::has_order_by_id and read the order from "
        "async_working_orders or async_order_history instead"
      );
    }
  }

private:
  // Initiates one client call; normalization and the handler run once, on the handler's executor.
  template <typename Out, typename Token, typename Call, typename Normalize>
  static auto translate(Token&& token, Call call, Normalize normalize) {
    return net::async_initiate<Token, void(Result<Out>)>(
      [call = std::move(call), normalize = std::move(normalize)](auto&& handler) mutable {
        auto executor = net::get_associated_executor(handler);
        call(
          net::bind_executor(
            executor,
            [
              handler = std::forward<decltype(handler)>(handler),
              normalize = std::move(normalize)
            ](auto raw) mutable {
              std::move(handler)(normalize(std::move(raw)));
            }
          )
        );
      },
      token
    );
  }

  // Completes with a pre-flight error, posted (never inline) to the handler's executor, or the
  // system executor if it has none.
  template <typename Out, typename Token>
  static auto complete_error(Token&& token, BrokerError error) {
    return net::async_initiate<Token, void(Result<Out>)>(
      [error = std::move(error)](auto&& handler) mutable {
        auto executor = net::get_associated_executor(handler, net::system_executor());
        net::post(
          executor,
          [
            handler = std::forward<decltype(handler)>(handler),
            error = std::move(error)
          ]() mutable {
            std::move(handler)(Result<Out>(std::move(error)));
          }
        );
      },
      token
    );
  }

  std::shared_ptr<Client> client_;
  Account account_;
};

/**
 * @brief A blocking broker client bound to one account, with the same operations as `BrokerSession`.
 *
 * Every operation returns `Result<T>`. An operation the broker does not offer does not compile.
 *
 * @tparam Client A blocking broker client (`BrokerTraits<Client>::blocking` is `true`).
 */
template <typename Client>
class BlockingSession {
public:
  /** @brief The client's traits. */
  using Traits = BrokerTraits<Client>;

  /** @brief The client's capabilities. */
  using Caps = BrokerCaps<Client>;

  /** @brief The broker's account selector type. */
  using Account = typename Traits::Account;

  /** @brief The broker's configuration type. */
  using Config = typename Traits::Config;

  /** @brief The broker's tag type. */
  using BrokerTag = typename Traits::BrokerTag;

  /** @brief The order reference type this session issues and accepts. */
  using Ref = OrderRef<BrokerTag>;

  /** @brief What an order-writing call returns: acknowledgement and reference. */
  using Placed = PlacedOrder<BrokerTag>;

  /** @brief One working order of this broker. */
  using Working = WorkingOrder<BrokerTag>;

  /** @brief A working-order listing of this broker. */
  using WorkingSet = WorkingOrders<BrokerTag>;

  static_assert(
    Traits::blocking,
    "BlockingSession requires a blocking client facade; use BrokerSession for an "
    "asynchronous client"
  );

  /**
   * @brief Binds a blocking client and an account.
   * @param client The client; must not be null.
   * @param account The account to act for.
   * @throws std::invalid_argument if `client` is null.
   */
  BlockingSession(std::shared_ptr<Client> client, Account account)
  : client_(std::move(client)), account_(std::move(account)) {
    if (!client_) {
      throw std::invalid_argument("broker session requires a client");
    }
  }

  /**
   * @brief Returns the broker's name.
   * @return `Traits::name`.
   */
  static constexpr std::string_view broker_name() noexcept { return Traits::name; }

  /**
   * @brief Returns the bound account.
   * @return The account selector.
   */
  const Account& account() const noexcept { return account_; }

  /**
   * @brief Returns the broker client, for broker-specific operations the session does not offer.
   * @return The client.
   */
  Client& client() noexcept { return *client_; }

  /**
   * @brief Returns the broker client.
   * @return The client.
   */
  const Client& client() const noexcept { return *client_; }

  /**
   * @brief Returns the client's request-pacing counters.
   *
   * A rising `delayed_` on a polling group while `TRADING` stays undelayed means trading capacity
   * is being kept free; a rising `shed_` means the queue is full. Safe to read between calls.
   *
   * @return The counters, or `nullptr` if the client is not paced.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    return Traits::rate_limit_stats(*client_);
  }

  /**
   * @brief Returns the current quote for an instrument.
   * @param instrument The instrument.
   * @return The quote, or an error.
   */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    return Traits::quote(*client_, std::move(instrument));
  }

  /**
   * @brief Returns the account's holdings.
   * @return The holdings, or an error.
   */
  Result<HoldingsSnapshot> holdings() { return Traits::holdings(*client_, account_); }

  /**
   * @brief Returns what the account may spend in a currency; requires `Caps::has_buying_power`.
   * @param currency The currency.
   * @return The buying power, or an error.
   */
  Result<BuyingPower> buying_power(Currency currency) {
    if constexpr (Caps::has_buying_power) {
      return Traits::buying_power(*client_, account_, currency);
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not answer what the account may spend; gate the call with "
        "BrokerCaps<Client>::has_buying_power"
      );
    }
  }

  /**
   * @brief Returns how much of an instrument the account may sell; only where `Caps::sellable_source` is `INQUIRY`.
   * @param instrument The instrument.
   * @return The quantity, or an error.
   */
  Result<Decimal64> sellable_quantity(InstrumentId instrument) {
    if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
      return Traits::sellable_quantity(*client_, account_, std::move(instrument));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker has no separate sellable-quantity inquiry; read "
        "HoldingPosition::sellable_quantity_ from the holdings snapshot, and gate on "
        "BrokerCaps<Client>::sellable_source"
      );
    }
  }

  /**
   * @brief Returns the account's commission rates; requires `Caps::has_commission_schedule`.
   * @return The schedule, or an error.
   */
  Result<CommissionSchedule> commissions() {
    if constexpr (Caps::has_commission_schedule) {
      return Traits::commissions(*client_, account_);
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker publishes no commission schedule; gate the call with "
        "BrokerCaps<Client>::has_commission_schedule"
      );
    }
  }

  /**
   * @brief Returns an instrument's reference data; requires `Caps::has_reference_data`.
   * @param subject The instrument.
   * @return The reference data, or an error.
   */
  Result<Instrument> instrument(InstrumentId subject) {
    if constexpr (Caps::has_reference_data) {
      return Traits::instrument(*client_, std::move(subject));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish instrument reference data; gate the call with "
        "BrokerCaps<Client>::has_reference_data"
      );
    }
  }

  /**
   * @brief Returns the trading calendar; requires `Caps::has_calendar`.
   * @param country The market.
   * @param from The date the answer is anchored to; empty means today.
   * @return The calendar, or an error.
   */
  Result<TradingCalendar> calendar(Country country, std::optional<Date> from = std::nullopt) {
    if constexpr (Caps::has_calendar) {
      if (auto invalid = Traits::validate_calendar(country, from)) {
        return Result<TradingCalendar>(std::move(*invalid));
      }
      return Traits::calendar(*client_, country, from);
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish a trading calendar; gate the call with "
        "BrokerCaps<Client>::has_calendar"
      );
    }
  }

  /**
   * @brief Returns an instrument's price limits for today; requires `Caps::has_price_limits`.
   * @param instrument The instrument.
   * @return The limits, or an error.
   */
  Result<PriceLimits> price_limits(InstrumentId instrument) {
    if constexpr (Caps::has_price_limits) {
      return Traits::price_limits(*client_, std::move(instrument));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish price limits; gate the call with "
        "BrokerCaps<Client>::has_price_limits"
      );
    }
  }

  /**
   * @brief Returns recent trades for an instrument, newest first; requires `Caps::has_trade_prints`.
   * @param instrument The instrument.
   * @param count How many trades to request.
   * @return The trades, or an error.
   */
  Result<std::vector<TradePrint>> trades(InstrumentId instrument, int count) {
    if constexpr (Caps::has_trade_prints) {
      return Traits::trades(*client_, std::move(instrument), count);
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish recent executions; gate the call with "
        "BrokerCaps<Client>::has_trade_prints"
      );
    }
  }

  /**
   * @brief Returns quotes for several instruments in one request; requires `Caps::has_batch_prices`.
   * @param instruments The instruments.
   * @return The quotes, or an error.
   */
  Result<std::vector<QuoteSnapshot>> prices(std::vector<InstrumentId> instruments) {
    if constexpr (Caps::has_batch_prices) {
      return Traits::prices(*client_, std::move(instruments));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker quotes one instrument per request; gate the call with "
        "BrokerCaps<Client>::has_batch_prices and call quote per instrument"
      );
    }
  }

  /**
   * @brief Returns an instrument's order book, best levels first; requires `Caps::has_depth`.
   * @param instrument The instrument.
   * @return The book, or an error.
   */
  Result<DepthSnapshot> depth(InstrumentId instrument) {
    if constexpr (Caps::has_depth) {
      return Traits::depth(*client_, std::move(instrument));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish an order book; gate the call with "
        "BrokerCaps<Client>::has_depth"
      );
    }
  }

  /**
   * @brief Returns bars for an instrument, oldest first, paging until `count_` bars are read.
   * @param query What to fetch.
   * @return The bars, or an error (an unsupported interval is refused before any I/O).
   */
  Result<BarSeries> bars(BarQuery query) {
    if (auto invalid = Traits::validate_bars(query)) {
      return Result<BarSeries>(std::move(*invalid));
    }
    return Traits::bars(*client_, std::move(query));
  }

  /**
   * @brief Places a new order.
   *
   * @warning A `TRANSPORT` error leaves the outcome unknown: the request may have reached the broker.
   *   The request is never resent automatically. Check working orders or order history before
   *   sending it again.
   * @param order The order; refused before any I/O as for `BrokerSession::async_place`.
   * @return The acknowledgement and reference, or an error.
   */
  Result<Placed> place(OrderRequest order) {
    if (auto refused = port_detail::prepare_place<Client>(order)) {
      return Result<Placed>(std::move(*refused));
    }
    return Traits::normalize_place(Traits::place_order(*client_, account_, std::move(order)));
  }

  /**
   * @brief Modifies a working order.
   *
   * @warning A `TRANSPORT` error leaves the outcome unknown: the request may have reached the broker.
   *   The request is never resent automatically. Check working orders or order history before
   *   sending it again.
   * @param ref The order.
   * @param instruction What to change; build it with `replace_with` or `reprice`.
   * @return The acknowledgement and reference, or an error.
   */
  Result<Placed> replace(const Ref& ref, const AmendInstruction& instruction) {
    auto amendment = port_detail::prepare_replace<Client>(ref, instruction);
    if (!amendment) {
      return Result<Placed>(std::move(amendment.error()));
    }
    return Traits::normalize_place(
      Traits::replace_order(*client_, account_, std::move(amendment.value()))
    );
  }

  /**
   * @brief Cancels a working order, in full or in part.
   *
   * @warning A `TRANSPORT` error leaves the outcome unknown: the request may have reached the broker.
   *   The request is never resent automatically. Check working orders or order history before
   *   sending it again.
   * @param ref The order.
   * @param instruction Build it with `cancel_all` or `cancel_quantity`; a trigger in it is never
   *   refused.
   * @return The acknowledgement and reference, or an error.
   */
  Result<Placed> cancel(const Ref& ref, const AmendInstruction& instruction) {
    auto amendment = Traits::to_cancellation(ref, instruction);
    if (!amendment) {
      return Result<Placed>(std::move(amendment.error()));
    }
    return Traits::normalize_place(
      Traits::cancel_order(*client_, account_, std::move(amendment.value()))
    );
  }

  /**
   * @brief Returns the orders still open to modification or cancellation; requires `Caps::has_working_orders`.
   * @return The listing, or an error.
   */
  Result<WorkingSet> working_orders() {
    if constexpr (Caps::has_working_orders) {
      return Traits::normalize_working_orders(Traits::working_orders(*client_, account_));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish its amendable orders; gate the call with "
        "BrokerCaps<Client>::has_working_orders"
      );
    }
  }

  /**
   * @brief Returns order history: placed orders and their executions.
   * @param query The filter.
   * @return One page, or an error (a filter the broker cannot apply is refused before any I/O).
   */
  Result<OrderPage> order_history(const OrderHistoryQuery& query) {
    auto converted = Traits::to_history_query(account_, query);
    if (!converted) {
      return Result<OrderPage>(std::move(converted.error()));
    }
    return Traits::order_history(*client_, account_, std::move(converted.value()));
  }

  /**
   * @brief Returns one order by its reference; requires `Caps::has_order_by_id`.
   * @param ref The order.
   * @return The order, or an error.
   */
  Result<OrderSnapshot> order(const Ref& ref) {
    if constexpr (Caps::has_order_by_id) {
      return Traits::order_by_id(*client_, account_, ref.primary_);
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not retrieve one order by identifier; gate the call with "
        "BrokerCaps<Client>::has_order_by_id and read the order from working_orders or "
        "order_history instead"
      );
    }
  }

  /**
   * @brief Lists the broker's accounts; requires `Caps::has_account_discovery`.
   * @return The accounts, or an error.
   */
  Result<std::vector<AccountInfo<Client>>> accounts() {
    if constexpr (Caps::has_account_discovery) {
      return port_detail::to_account_info<Client>(Traits::accounts(*client_));
    } else {
      static_assert(
        port_detail::dependent_false<Client>,
        "this broker does not publish selectable accounts; its account selector comes from "
        "configuration, so gate the call with BrokerCaps<Client>::has_account_discovery"
      );
    }
  }

private:
  std::shared_ptr<Client> client_;
  Account account_;
};

/**
 * @brief Lists a broker's accounts, before a session exists; requires
 *   `BrokerCaps<Client>::has_account_discovery` (KIS reads its account from configuration).
 *
 * @tparam Client An asynchronous broker client type.
 * @tparam CompletionToken A completion token for `void(Result<std::vector<AccountInfo<Client>>>)`.
 * @param client The client.
 * @param token The completion token; the handler runs once, on its associated executor.
 * @return Whatever `token` produces.
 */
template <typename Client, typename CompletionToken>
auto async_accounts(Client& client, CompletionToken&& token) {
  if constexpr (BrokerCaps<Client>::has_account_discovery) {
    using Traits = BrokerTraits<Client>;
    using Accounts = std::vector<AccountInfo<Client>>;
    return net::async_initiate<CompletionToken, void(Result<Accounts>)>(
      [&client](auto&& handler) mutable {
        auto executor = net::get_associated_executor(handler);
        Traits::accounts(
          client,
          net::bind_executor(
            executor,
            [handler = std::forward<decltype(handler)>(handler)](auto raw) mutable {
              std::move(handler)(port_detail::to_account_info<Client>(std::move(raw)));
            }
          )
        );
      },
      token
    );
  } else {
    static_assert(
      port_detail::dependent_false<Client>,
      "this broker does not publish selectable accounts; its account selector comes from "
      "configuration, so gate the call with BrokerCaps<Client>::has_account_discovery"
    );
  }
}

/**
 * @brief Lists a broker's accounts with a blocking client, before a session exists; requires
 *   `BrokerCaps<Client>::has_account_discovery`.
 *
 * @tparam Client A blocking broker client type.
 * @param client The client.
 * @return The accounts, or an error.
 */
template <typename Client>
Result<std::vector<AccountInfo<Client>>> list_accounts(Client& client) {
  if constexpr (BrokerCaps<Client>::has_account_discovery) {
    return port_detail::to_account_info<Client>(BrokerTraits<Client>::accounts(client));
  } else {
    static_assert(
      port_detail::dependent_false<Client>,
      "this broker does not publish selectable accounts; its account selector comes from "
      "configuration, so gate the call with BrokerCaps<Client>::has_account_discovery"
    );
  }
}

} // namespace algotradelib
