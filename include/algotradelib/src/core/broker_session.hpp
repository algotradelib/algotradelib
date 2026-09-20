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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {

namespace port_detail {

/**
 * Converts a broker's own account listing into the normalized listing.
 *
 * The conversion is shared by the asynchronous and blocking entry points so a
 * broker's traits describe one account item in one place.
 */
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

} // namespace port_detail

/**
 * One broker bound to one account, exposing a verb set that names no vendor type.
 *
 * A session is the port strategy-side code is written against. Selecting a
 * broker is choosing `Client`; binding the account at construction is what
 * removes `kis::KisAccount` and Toss's account sequence from every call site,
 * which is the largest single source of signature divergence between the two
 * clients.
 *
 * Completion-signature translation lives here and only here. Each verb runs
 * `net::async_initiate` with the port's normalized signature, invokes the
 * trait's raw thunk with an inner handler bound to the caller handler's
 * associated executor, and applies the trait's pure normalizer to the result.
 * The guarantee the concrete clients already give — every completion runs once,
 * on the caller handler's associated executor — therefore holds for the port
 * through code written and reviewed one time rather than once per broker.
 *
 * A session is cheap to copy: it holds a shared client pointer and the bound
 * account selector.
 */
template <typename Client>
class BrokerSession {
public:
  /** Adaptation point for the bound client. */
  using Traits = BrokerTraits<Client>;

  /** Compile-time feature set of the bound client. */
  using Caps = BrokerCaps<Client>;

  /** Broker-specific account selector type. */
  using Account = typename Traits::Account;

  /** Broker-specific configuration type. */
  using Config = typename Traits::Config;

  /** Compile-time identity of the bound broker. */
  using BrokerTag = typename Traits::BrokerTag;

  /** Opaque order reference this session issues and accepts. */
  using Ref = OrderRef<BrokerTag>;

  /** Acknowledgement and reference returned by an order-writing verb. */
  using Placed = PlacedOrder<BrokerTag>;

  /** One amendable order of this broker. */
  using Working = WorkingOrder<BrokerTag>;

  /** Amendable orders of this broker. */
  using WorkingSet = WorkingOrders<BrokerTag>;

  ATL_ASSERT_BROKER(Client);

  static_assert(
    !Traits::blocking,
    "BrokerSession requires an asynchronous client; use BlockingSession for a blocking facade"
  );

  /**
   * Binds one client and one account.
   *
   * @throws std::invalid_argument if `client` is null.
   */
  BrokerSession(std::shared_ptr<Client> client, Account account)
  : client_(std::move(client)), account_(std::move(account)) {
    if (!client_) {
      throw std::invalid_argument("broker session requires a client");
    }
  }

  /** Returns the normalized broker name this session is bound to. */
  static constexpr std::string_view broker_name() noexcept { return Traits::name; }

  /** Returns the bound account selector. */
  const Account& account() const noexcept { return account_; }

  /**
   * Returns the concrete client.
   *
   * This is the deliberate escape hatch for a broker-specific operation the port
   * does not normalize. Using it inside an `if constexpr` branch on
   * `BrokerCaps<Client>` keeps the broker-specific path visible in the source
   * and compiled only where the feature exists.
   */
  Client& client() noexcept { return *client_; }

  /** Returns the concrete client. */
  const Client& client() const noexcept { return *client_; }

  /** Returns the shared client pointer, for constructing a second session. */
  const std::shared_ptr<Client>& shared_client() const noexcept { return client_; }

  /**
   * Returns how this broker's connection has been paced, or null when unpaced.
   *
   * A runner reads this to tell whether it is asking for more capacity than it
   * has: rising `delayed_` on a polling group while `TRADING` stays undelayed is
   * the reservation doing its job, and rising `shed_` means the queue is full.
   *
   * The counters live on the client's strand, so read them from a completion
   * handler; on a blocking session any moment between calls is safe.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    return Traits::rate_limit_stats(*client_);
  }

  /** Cancels transport work and queued operations on the bound client. */
  void close() { client_->close(); }

  /** Requests and normalizes a current quote. */
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

  /** Requests and normalizes all holdings for the bound account. */
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
   * Submits a new order and returns its acknowledgement and reference.
   *
   * The order POST is never replayed automatically, by the client or by the
   * port. A `TRANSPORT` error therefore leaves the outcome *indeterminate*: the
   * order may or may not have reached the broker. Reconcile through
   * `async_working_orders` or `async_order_history` before resubmitting.
   *
   * An idempotency key is rejected before any I/O on a broker that does not
   * honor one, because a caller who supplied one would otherwise believe a
   * resubmission was safe when it is not.
   */
  template <typename CompletionToken>
  auto async_place(OrderRequest order, CompletionToken&& token) {
    if (auto invalid = Traits::validate_place(order)) {
      return complete_error<Placed>(
        std::forward<CompletionToken>(token), std::move(*invalid)
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
   * Modifies the shape or quantity of an existing order.
   *
   * The indeterminacy rule of `async_place` applies unchanged: a transport
   * failure does not mean the modification was rejected.
   */
  template <typename CompletionToken>
  auto async_replace(Ref ref, AmendInstruction instruction, CompletionToken&& token) {
    auto amendment = Traits::to_replacement(ref, instruction);
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
   * Cancels an existing order.
   *
   * The indeterminacy rule of `async_place` applies unchanged: a transport
   * failure does not mean the order is still live.
   *
   * Build `instruction` with `cancel_all()` or `cancel_quantity()` on the
   * `WorkingOrder`, which supplies whatever the bound broker requires. A partial
   * cancellation is rejected before any I/O on a broker that cancels in full.
   *
   * `Ref` is typed by broker, so a reference issued by another broker is a
   * compile error rather than a run-time rejection.
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
   * Retrieves every order still open to modification or cancellation.
   *
   * Whether the broker *offers* this inquiry is `BrokerCaps::has_working_orders`.
   * Whether the bound configuration may *use* it remains a run-time error: KIS
   * does not serve it to demo accounts and reports `INVALID_ARGUMENT`.
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
   * Retrieves placed orders and their executions.
   *
   * A filter the bound broker cannot express is rejected with
   * `INVALID_ARGUMENT` before any I/O rather than dropped, so a listing never
   * answers a different question from the one asked.
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
   * Retrieves bars for one instrument, paging until `count_` is satisfied.
   *
   * Bars arrive oldest first whatever order the broker sent them in, and carry the
   * stamp convention the broker uses, because a timestamp alone cannot say whether
   * it marks a period's open or its close.
   *
   * An interval the bound broker does not publish is refused before any I/O rather
   * than produced by aggregating a shorter one; `BrokerCaps<Client>::supports_interval`
   * answers the same question at compile time.
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

  /** Retrieves one order by reference, on a broker that addresses one directly. */
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
  /**
   * Initiates one client call and translates its completion signature.
   *
   * The inner handler is bound to the caller handler's associated executor, so
   * normalization and the user handler both run there, exactly once.
   */
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

  /**
   * Completes with a pre-flight error without contacting the broker.
   *
   * The handler is posted rather than invoked inline, so a rejection never
   * completes before the initiating call returns, which is the same guarantee a
   * broker-side failure already gives.
   *
   * A rejection happens before any client work, so there is no broker strand to
   * complete on. The caller handler's own associated executor is used when it has
   * one, and the system executor is the fallback when it does not.
   */
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
 * The same verb set over a blocking client facade.
 *
 * Every verb returns `Result<T>` directly and reuses the identical trait
 * normalizers, so the blocking and asynchronous ports cannot drift apart in how
 * they normalize a broker response.
 */
template <typename Client>
class BlockingSession {
public:
  /** Adaptation point for the bound client. */
  using Traits = BrokerTraits<Client>;

  /** Compile-time feature set of the bound client. */
  using Caps = BrokerCaps<Client>;

  /** Broker-specific account selector type. */
  using Account = typename Traits::Account;

  /** Broker-specific configuration type. */
  using Config = typename Traits::Config;

  /** Compile-time identity of the bound broker. */
  using BrokerTag = typename Traits::BrokerTag;

  /** Opaque order reference this session issues and accepts. */
  using Ref = OrderRef<BrokerTag>;

  /** Acknowledgement and reference returned by an order-writing verb. */
  using Placed = PlacedOrder<BrokerTag>;

  /** One amendable order of this broker. */
  using Working = WorkingOrder<BrokerTag>;

  /** Amendable orders of this broker. */
  using WorkingSet = WorkingOrders<BrokerTag>;

  ATL_ASSERT_BROKER(Client);

  static_assert(
    Traits::blocking,
    "BlockingSession requires a blocking client facade; use BrokerSession for an "
    "asynchronous client"
  );

  /**
   * Binds one blocking client and one account.
   *
   * @throws std::invalid_argument if `client` is null.
   */
  BlockingSession(std::shared_ptr<Client> client, Account account)
  : client_(std::move(client)), account_(std::move(account)) {
    if (!client_) {
      throw std::invalid_argument("broker session requires a client");
    }
  }

  /** Returns the normalized broker name this session is bound to. */
  static constexpr std::string_view broker_name() noexcept { return Traits::name; }

  /** Returns the bound account selector. */
  const Account& account() const noexcept { return account_; }

  /** Returns the concrete client, as the broker-specific escape hatch. */
  Client& client() noexcept { return *client_; }

  /** Returns the concrete client. */
  const Client& client() const noexcept { return *client_; }

  /**
   * Returns how this broker's connection has been paced, or null when unpaced.
   *
   * A runner reads this to tell whether it is asking for more capacity than it
   * has: rising `delayed_` on a polling group while `TRADING` stays undelayed is
   * the reservation doing its job, and rising `shed_` means the queue is full.
   *
   * The counters live on the client's strand, so read them from a completion
   * handler; on a blocking session any moment between calls is safe.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    return Traits::rate_limit_stats(*client_);
  }

  /** Returns the current normalized quote. */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    return Traits::quote(*client_, std::move(instrument));
  }

  /** Returns all normalized holdings for the bound account. */
  Result<HoldingsSnapshot> holdings() { return Traits::holdings(*client_, account_); }

  /** Returns bars for one instrument, paging until the query's count is satisfied. */
  Result<BarSeries> bars(BarQuery query) {
    if (auto invalid = Traits::validate_bars(query)) {
      return Result<BarSeries>(std::move(*invalid));
    }
    return Traits::bars(*client_, std::move(query));
  }

  /** Submits a new order. The indeterminacy rule of `BrokerSession::async_place` applies. */
  Result<Placed> place(OrderRequest order) {
    if (auto invalid = Traits::validate_place(order)) {
      return Result<Placed>(std::move(*invalid));
    }
    return Traits::normalize_place(Traits::place_order(*client_, account_, std::move(order)));
  }

  /** Modifies an existing order. */
  Result<Placed> replace(const Ref& ref, const AmendInstruction& instruction) {
    auto amendment = Traits::to_replacement(ref, instruction);
    if (!amendment) {
      return Result<Placed>(std::move(amendment.error()));
    }
    return Traits::normalize_place(
      Traits::replace_order(*client_, account_, std::move(amendment.value()))
    );
  }

  /** Cancels an existing order. */
  Result<Placed> cancel(const Ref& ref, const AmendInstruction& instruction) {
    auto amendment = Traits::to_cancellation(ref, instruction);
    if (!amendment) {
      return Result<Placed>(std::move(amendment.error()));
    }
    return Traits::normalize_place(
      Traits::cancel_order(*client_, account_, std::move(amendment.value()))
    );
  }

  /** Returns every order still open to modification or cancellation. */
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

  /** Returns placed orders and their executions. */
  Result<OrderPage> order_history(const OrderHistoryQuery& query) {
    auto converted = Traits::to_history_query(account_, query);
    if (!converted) {
      return Result<OrderPage>(std::move(converted.error()));
    }
    return Traits::order_history(*client_, account_, std::move(converted.value()));
  }

  /** Returns one order by reference, on a broker that addresses one directly. */
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
   * Lists the accounts this broker publishes.
   *
   * Available only where the broker publishes a list; a broker whose selector
   * comes from configuration does not compile this call.
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
 * Lists the accounts a broker publishes, on a broker that publishes them.
 *
 * Account discovery is a free function rather than a session verb because it
 * answers what a session should be bound *to*, and therefore runs before a
 * session exists. On a broker whose selector is local — KIS reads it from
 * configuration — the capability is false and this function does not compile.
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
 * Lists the accounts a blocking client's broker publishes.
 *
 * Account discovery answers what a session should be bound *to*, so it runs
 * before a session exists. This is the blocking counterpart of
 * `async_accounts`.
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
