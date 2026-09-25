#pragma once

#include "./config.hpp"
#include "../common.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib::tossinvest {

/** Toss brokerage account returned by account discovery. */
struct TossAccount {
  std::string account_number_;
  std::int64_t sequence_{0};
  std::string account_type_;
};

/**
 * Lifecycle group selected by a Toss order listing.
 *
 * Toss groups order states rather than listing them individually: `OPEN`
 * covers pending, partially filled, and pending amendment states, while
 * `CLOSED` covers terminal ones. The group is not the same value space as an
 * individual order's `OrderStatus`.
 */
enum class TossOrderScope {
  OPEN,
  CLOSED,
};

/**
 * Filter for one Toss order listing.
 *
 * Toss ignores `cursor_` and `limit_` for `TossOrderScope::OPEN`, which always
 * returns every pending order in one response. They are still accepted here so
 * one query type serves both scopes, and the adapter simply omits them where
 * the broker states they have no effect.
 */
struct TossOrderQuery {
  std::int64_t account_sequence_{0};
  TossOrderScope scope_{TossOrderScope::OPEN};
  std::optional<std::string> symbol_;
  std::optional<Date> from_;
  std::optional<Date> to_;
  std::optional<std::string> cursor_;
  std::optional<int> limit_;
};

/**
 * Price and quantity instruction for a Toss order modification.
 *
 * Toss requires `quantity_` for Korean orders and forbids it for United States
 * orders, where only the price may change. The caller states the intent and
 * the adapter transmits it unchanged rather than inferring one.
 */
struct TossAmendment {
  std::string order_id_;
  OrderType type_{OrderType::LIMIT};
  std::optional<Decimal64> quantity_;
  std::optional<Decimal64> price_;
  bool confirm_high_value_{false};
};

/**
 * Reads the retry delay Toss publishes with a rate-limit refusal.
 *
 * `Retry-After` is documented on Toss's `429` response. It is read here rather
 * than in shared broker code because which headers carry meaning is each
 * broker's own contract: KIS publishes neither this one nor any equivalent, and a
 * future broker will publish its own set.
 */
inline std::optional<std::chrono::seconds> retry_after(const network::HttpResponse& response) {
  const auto seconds = broker_detail::integer_header(response, "Retry-After");
  if (!seconds) {
    return std::nullopt;
  }
  return std::chrono::seconds(*seconds);
}

/**
 * Reads the allowance Toss says it is enforcing, in requests per second.
 *
 * Toss documents `X-RateLimit-Limit` as the burst capacity per second on the same
 * refusal. It is observed when present and never required, because the header is
 * documented on the refusal rather than promised on a successful response.
 */
inline std::optional<int> reported_rate_limit(const network::HttpResponse& response) {
  const auto limit = broker_detail::integer_header(response, "X-RateLimit-Limit");
  if (!limit || *limit < 1 || *limit > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(*limit);
}

/**
 * Returns an error when Toss does not publish bars at `interval`.
 *
 * Toss publishes one-minute and one-day bars only. An interval it does not publish
 * is refused rather than produced by aggregating a shorter one, which would answer
 * a different question from the one asked.
 *
 * Both the client and the broker port call this, so the rule has one definition.
 */
inline std::optional<BrokerError> unsupported_bar_interval(BarInterval interval) {
  if (interval == BarInterval::ONE_MINUTE || interval == BarInterval::ONE_DAY) {
    return std::nullopt;
  }
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "unsupported_interval";
  error.message_ =
    "Toss publishes one-minute and one-day bars only; gate on "
    "BrokerCaps<Client>::supports_interval before asking for another";
  return error;
}

/** Persistent asynchronous Toss Securities client with coalesced OAuth refresh. */
class TossClient : public std::enable_shared_from_this<TossClient> {
public:
  /** Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;

  /** Completion handler for one normalized bar series. */
  using BarsHandler = net::any_completion_handler<void(Result<BarSeries>)>;

  /** Completion handler for one normalized order book. */
  using DepthHandler = net::any_completion_handler<void(Result<DepthSnapshot>)>;

  /** Completion handler for one instrument's price bounds. */
  using PriceLimitsHandler = net::any_completion_handler<void(Result<PriceLimits>)>;

  /** Completion handler for recent executions. */
  using TradesHandler = net::any_completion_handler<void(Result<std::vector<TradePrint>>)>;

  /** Completion handler for several instruments quoted at once. */
  using PricesHandler = net::any_completion_handler<void(Result<std::vector<QuoteSnapshot>>)>;

  /** Completion handler for one trading calendar. */
  using CalendarHandler = net::any_completion_handler<void(Result<TradingCalendar>)>;

  /** Completion handler for one instrument's reference data. */
  using InstrumentHandler = net::any_completion_handler<void(Result<Instrument>)>;
  /** Completion handler for Toss account discovery. */
  using AccountsHandler = net::any_completion_handler<void(Result<std::vector<TossAccount>>)>;

  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;
  /** Completion handler for one normalized buying-power reading. */
  using BuyingPowerHandler = net::any_completion_handler<void(Result<BuyingPower>)>;
  /** Completion handler for one instrument's sellable quantity. */
  using SellableQuantityHandler = net::any_completion_handler<void(Result<Decimal64>)>;
  /** Completion handler for the account's commission schedule. */
  using CommissionScheduleHandler = net::any_completion_handler<void(Result<CommissionSchedule>)>;

  /** Completion handler for one accepted Toss order instruction. */
  using OrderAckHandler = net::any_completion_handler<void(Result<OrderAck>)>;

  /** Completion handler for one normalized page of orders. */
  using OrderPageHandler = net::any_completion_handler<void(Result<OrderPage>)>;

  /** Completion handler for one normalized order. */
  using OrderHandler = net::any_completion_handler<void(Result<OrderSnapshot>)>;

  /** Internal completion handler for a shared access-token refresh. */
  using TokenHandler = net::any_completion_handler<void(std::optional<BrokerError>)>;

  /**
   * Constructs a client and takes value ownership of its credentials.
   *
   * Lvalue configurations are copied once; rvalue configurations are moved.
   */
  TossClient(
    net::any_io_executor executor,
    ssl::context& context,
    TossConfig config,
    network::HttpClientOptions options = {}
  )
  : strand_(net::make_strand(std::move(executor))), config_(std::move(config)),
    http_(
      std::make_shared<network::AsyncHttpClient>(strand_, context, "openapi.tossinvest.com", "443", options)
    ) {}

  /**
   * Requests bars, following Toss's cursor until `count_` is satisfied.
   *
   * Toss returns at most 200 bars per page, newest first, with `nextBefore` as the
   * cursor for the page before. The adapter pages backwards and reverses once at
   * the end, so the caller receives ascending bars however many pages it took.
   */
  template <typename CompletionToken>
  auto async_bars(BarQuery query, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<BarSeries>)>(
      [self = shared_from_this(), query = std::move(query)](auto&& handler) mutable {
        auto completion = BarsHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, query = std::move(query), completion = std::move(completion)]() mutable {
            self->start_bars(std::move(query), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Requests the trading calendar around `from`.
   *
   * Toss answers with three days — the requested one and the business days either
   * side of it — rather than a span, so the calendar returned covers exactly those
   * three. A caller needing more asks again from a different date.
   */
  template <typename CompletionToken>
  auto async_calendar(Country country, std::optional<Date> from, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<TradingCalendar>)>(
      [self = shared_from_this(), country, from](auto&& handler) mutable {
        auto completion = CalendarHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, country, from, completion = std::move(completion)]() mutable {
            self->start_calendar(country, from, std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Requests static reference data for one instrument. */
  template <typename CompletionToken>
  auto async_instrument(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<Instrument>)>(
      [self = shared_from_this(), instrument = std::move(instrument)](auto&& handler) mutable {
        auto completion = InstrumentHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion)]() mutable {
            self->start_market_read(
              std::move(instrument),
              "/api/v1/stocks?symbols=",
              std::move(completion),
              parse_instrument
            );
          }
        );
      },
      token
    );
  }

  /** Requests today's upper and lower price bound for one instrument. */
  template <typename CompletionToken>
  auto async_price_limits(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<PriceLimits>)>(
      [self = shared_from_this(), instrument = std::move(instrument)](auto&& handler) mutable {
        auto completion = PriceLimitsHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion)]() mutable {
            self->start_market_read(
              std::move(instrument),
              "/api/v1/price-limits?symbol=",
              std::move(completion),
              parse_price_limits
            );
          }
        );
      },
      token
    );
  }

  /** Requests recent executions for one instrument, newest first. */
  template <typename CompletionToken>
  auto async_trades(InstrumentId instrument, int count, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<std::vector<TradePrint>>)>(
      [self = shared_from_this(), instrument = std::move(instrument), count](
        auto&& handler
      ) mutable {
        auto completion = TradesHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), count, completion = std::move(completion)]()
            mutable {
            // Toss caps this at fifty prints; asking for more would be answered with
            // fifty anyway, so the cap is applied here rather than silently by the
            // broker.
            const int wanted = count < 1 ? 1 : (count > trade_limit_ ? trade_limit_ : count);
            self->start_market_read(
              std::move(instrument),
              "/api/v1/trades?count=" + std::to_string(wanted) + "&symbol=",
              std::move(completion),
              parse_trades
            );
          }
        );
      },
      token
    );
  }

  /**
   * Quotes up to two hundred instruments in one request.
   *
   * The whole point of the call is that it is one request rather than many, so a
   * list longer than Toss accepts is refused rather than split: splitting would
   * quietly spend several times the rate-limit budget the caller was economising.
   */
  template <typename CompletionToken>
  auto async_prices(std::vector<InstrumentId> instruments, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<std::vector<QuoteSnapshot>>)>(
      [self = shared_from_this(), instruments = std::move(instruments)](auto&& handler) mutable {
        auto completion = PricesHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instruments = std::move(instruments), completion = std::move(completion)]()
            mutable {
            self->start_prices(std::move(instruments), std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Requests and normalizes the resting orders on both sides of the book. */
  template <typename CompletionToken>
  auto async_depth(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<DepthSnapshot>)>(
      [self = shared_from_this(), instrument = std::move(instrument)](auto&& handler) mutable {
        auto completion = DepthHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion)]() mutable {
            self->start_depth(std::move(instrument), std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Requests and normalizes a current quote. */
  template <typename CompletionToken>
  auto async_quote(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<QuoteSnapshot>)>(
      [self = shared_from_this(), instrument = std::move(instrument)](auto&& handler) mutable {
        auto completion = QuoteHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion)]() mutable {
            self->start_quote(std::move(instrument), std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Discovers the brokerage accounts available to the current credentials. */
  template <typename CompletionToken>
  auto async_accounts(CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<std::vector<TossAccount>>)>(
      [self = shared_from_this()](auto&& handler) mutable {
        auto completion = AccountsHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, completion = std::move(completion)]() mutable {
            self->start_accounts(std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Requests what the account may spend in `currency`.
   *
   * The currency is required by the endpoint rather than defaulted, and a currency Toss does not
   * price is refused before the socket write instead of being sent and rejected.
   */
  template <typename CompletionToken>
  auto async_buying_power(Currency currency, std::int64_t account_sequence, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<BuyingPower>)>(
      [self = shared_from_this(), currency, account_sequence](auto&& handler) mutable {
        auto completion = BuyingPowerHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, currency, account_sequence, completion = std::move(completion)]() mutable {
            self->start_buying_power(currency, account_sequence, std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Requests how much of one instrument the account may sell. */
  template <typename CompletionToken>
  auto async_sellable_quantity(
    InstrumentId instrument,
    std::int64_t account_sequence,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<Decimal64>)>(
      [self = shared_from_this(), instrument = std::move(instrument), account_sequence](
        auto&& handler
      ) mutable {
        auto completion = SellableQuantityHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), account_sequence,
           completion = std::move(completion)]() mutable {
            self->start_sellable_quantity(
              std::move(instrument), account_sequence, std::move(completion)
            );
          }
        );
      },
      token
    );
  }

  /** Requests the commission rates in force for the account. */
  template <typename CompletionToken>
  auto async_commissions(std::int64_t account_sequence, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<CommissionSchedule>)>(
      [self = shared_from_this(), account_sequence](auto&& handler) mutable {
        auto completion = CommissionScheduleHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, account_sequence, completion = std::move(completion)]() mutable {
            self->start_commissions(account_sequence, std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Requests and normalizes holdings for one discovered account sequence. */
  template <typename CompletionToken>
  auto async_holdings(std::int64_t account_sequence, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<HoldingsSnapshot>)>(
      [self = shared_from_this(), account_sequence](auto&& handler) mutable {
        auto completion = HoldingsHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, account_sequence, completion = std::move(completion)]() mutable {
            self->start_holdings(account_sequence, std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Submits a new order and returns Toss's acknowledgement.
   *
   * The order POST is never replayed automatically. A `TRANSPORT` error
   * therefore leaves the outcome *indeterminate*: the order may or may not
   * have reached Toss. Supplying `OrderRequest::client_order_id_` makes a
   * deliberate resubmission safe for ten minutes, because Toss treats it as an
   * idempotency key and returns the original result. Without one, reconcile
   * through `async_orders` before resubmitting.
   */
  template <typename CompletionToken>
  auto async_place_order(
    std::int64_t account_sequence,
    OrderRequest order,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<OrderAck>)>(
      [self = shared_from_this(), account_sequence, order = std::move(order)](
        auto&& handler
      ) mutable {
        auto completion = OrderAckHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account_sequence,
            order = std::move(order),
            completion = std::move(completion)
          ]() mutable {
            self->start_place_order(account_sequence, std::move(order), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Modifies the price, and for Korean orders the quantity, of an open order.
   *
   * The same indeterminacy rule as `async_place_order` applies, and a
   * modification carries no idempotency key at all.
   */
  template <typename CompletionToken>
  auto async_replace_order(
    std::int64_t account_sequence,
    TossAmendment amendment,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<OrderAck>)>(
      [self = shared_from_this(), account_sequence, amendment = std::move(amendment)](
        auto&& handler
      ) mutable {
        auto completion = OrderAckHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account_sequence,
            amendment = std::move(amendment),
            completion = std::move(completion)
          ]() mutable {
            self->start_replace_order(
              account_sequence, std::move(amendment), std::move(completion)
            );
          }
        );
      },
      token
    );
  }

  /**
   * Cancels an open order.
   *
   * The same indeterminacy rule as `async_place_order` applies: a transport
   * failure does not mean the order is still live.
   */
  template <typename CompletionToken>
  auto async_cancel_order(
    std::int64_t account_sequence,
    std::string order_id,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<OrderAck>)>(
      [self = shared_from_this(), account_sequence, order_id = std::move(order_id)](
        auto&& handler
      ) mutable {
        auto completion = OrderAckHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account_sequence,
            order_id = std::move(order_id),
            completion = std::move(completion)
          ]() mutable {
            self->start_cancel_order(account_sequence, std::move(order_id), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Retrieves one page of orders in the requested lifecycle group.
   *
   * A `TossOrderScope::OPEN` listing returns every pending order and reports
   * no cursor. A `TossOrderScope::CLOSED` listing pages; pass the returned
   * `OrderPage::next_cursor_` back unchanged to continue.
   */
  template <typename CompletionToken>
  auto async_orders(TossOrderQuery query, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<OrderPage>)>(
      [self = shared_from_this(), query = std::move(query)](auto&& handler) mutable {
        auto completion = OrderPageHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, query = std::move(query), completion = std::move(completion)]() mutable {
            self->start_orders(std::move(query), std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Retrieves one order in any lifecycle state. */
  template <typename CompletionToken>
  auto async_order(std::int64_t account_sequence, std::string order_id, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<OrderSnapshot>)>(
      [self = shared_from_this(), account_sequence, order_id = std::move(order_id)](
        auto&& handler
      ) mutable {
        auto completion = OrderHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account_sequence,
            order_id = std::move(order_id),
            completion = std::move(completion)
          ]() mutable {
            self->start_order(account_sequence, std::move(order_id), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Returns this connection's pacing statistics, or null when it is unpaced.
   *
   * The counters live on the client's strand. Read them from a completion
   * handler, where that strand is already current; on a blocking facade any
   * moment between calls is safe, because its event loop is idle then.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    const auto* limiter = http_->rate_limits();
    return limiter ? &limiter->stats() : nullptr;
  }

  /** Cancels transport work and queued HTTP operations. */
  void close() { http_->close(); }

private:
  friend struct TossClientTestAccess;

  net::strand<net::any_io_executor> strand_;
  TossConfig config_;
  std::shared_ptr<network::AsyncHttpClient> http_;
  TossRest token_;
  bool refreshing_{false};
  std::deque<TokenHandler> token_waiters_;

  /**
   * One bar request spanning however many pages Toss needs to answer it.
   *
   * `descending_` accumulates pages exactly as Toss sends them — newest first,
   * each page older than the last — and is reversed once when the operation
   * finishes. Reversing per page, or inserting at the front, would do the same
   * work repeatedly for no benefit.
   */
  struct BarOperation {
    BarQuery query_;
    Currency currency_;
    std::vector<Bar> descending_;
    std::optional<std::string> before_;
    std::optional<std::string> last_cursor_;
    int pages_{0};
    BarsHandler completion_;

    BarOperation(BarQuery query, BarsHandler completion)
    : query_(std::move(query)), completion_(std::move(completion)) {
      if (query_.before_) {
        before_ = format_iso8601_utc(*query_.before_);
      }
    }
  };

  /** Largest number of bars Toss returns in one page. */
  static constexpr int bar_page_limit_ = 200;

  /**
   * Most pages one bar request may consume.
   *
   * A bound is needed because a broker that kept returning a cursor would
   * otherwise page forever. Fifty pages is 10,000 bars, far beyond any window a
   * caller asks for in one call, so reaching it means something is wrong rather
   * than that the caller was unlucky.
   */
  static constexpr int bar_page_budget_ = 50;

  /** Validates a bar request and starts paging. */
  void start_bars(BarQuery query, BarsHandler completion) {
    if (auto invalid = validate_bar_query(query)) {
      deliver(std::move(completion), Result<BarSeries>(std::move(*invalid)));
      return;
    }

    auto operation = std::make_shared<BarOperation>(std::move(query), std::move(completion));
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(operation->completion_), Result<BarSeries>(std::move(*token_error))
            );
            return;
          }
          self->request_bars(operation);
        }
      )
    );
  }

  /** Returns an error when Toss cannot answer the request as described. */
  static std::optional<BrokerError> validate_bar_query(const BarQuery& query) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (!broker_detail::valid_symbol(query.instrument_.symbol_)) {
      error.broker_code_ = "invalid_instrument";
      error.message_ = "bar request requires a symbol";
      return error;
    }
    if (query.count_ < 1) {
      error.broker_code_ = "invalid_count";
      error.message_ = "bar request must ask for at least one bar";
      return error;
    }
    return unsupported_bar_interval(query.interval_);
  }

  /** Returns Toss's code for a supported interval. */
  static const char* bar_interval_code(BarInterval interval) {
    return interval == BarInterval::ONE_MINUTE ? "1m" : "1d";
  }

  /** Sends the next page of a bar request. */
  void request_bars(const std::shared_ptr<BarOperation>& operation) {
    const int outstanding = operation->query_.count_ -
      static_cast<int>(operation->descending_.size());
    const int wanted = outstanding > bar_page_limit_ ? bar_page_limit_ : outstanding;

    std::string target = "/api/v1/candles?symbol=" +
      broker_detail::query_encode(operation->query_.instrument_.symbol_) +
      "&interval=" + bar_interval_code(operation->query_.interval_) +
      "&count=" + std::to_string(wanted) +
      "&adjusted=" + (operation->query_.adjusted_ ? "true" : "false");
    if (operation->before_) {
      target += "&before=" + broker_detail::query_encode(*operation->before_);
    }

    network::HttpRequest request;
    // Bars are the traffic a trading reserve exists to hold back.
    request.group_ = network::RateLimitGroup::MARKET_DATA;
    request.method_ = http::verb::get;
    request.target_ = std::move(target);
    request.headers_.emplace_back("Authorization", "Bearer " + token_.access_token_);

    ++operation->pages_;
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), operation](Result<network::HttpResponse> result) mutable {
        self->handle_bars_page(operation, std::move(result));
      }
    );
  }

  /** Accumulates one page and either continues or finishes the request. */
  void handle_bars_page(
    const std::shared_ptr<BarOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    std::optional<std::string> cursor;
    auto appended = parse_bars_page(
      std::move(result), operation->query_, operation->descending_, cursor, operation->currency_
    );
    if (appended) {
      deliver(std::move(operation->completion_), Result<BarSeries>(std::move(*appended)));
      return;
    }

    const int collected = static_cast<int>(operation->descending_.size());
    const bool satisfied = collected >= operation->query_.count_;
    const bool exhausted = !cursor || cursor->empty();
    const bool repeated = cursor && operation->last_cursor_ && *cursor == *operation->last_cursor_;
    const bool spent = operation->pages_ >= bar_page_budget_;
    if (satisfied || exhausted || repeated || spent) {
      finish_bars(operation, cursor);
      return;
    }

    operation->last_cursor_ = cursor;
    operation->before_ = cursor;
    request_bars(operation);
  }

  /**
   * Parses one page into the operation, or returns the error that ends it.
   *
   * A page that parses but contains no bar also ends the request: a broker that
   * returns an empty page while still reporting a cursor would otherwise be paged
   * forever, and `holdings-core` established the same guard.
   *
   * Takes the accumulation and cursor by reference rather than an operation, so the
   * normalization is a pure function of a response and can be tested as one.
   */
  static std::optional<BrokerError> parse_bars_page(
    Result<network::HttpResponse> result,
    const BarQuery& query,
    std::vector<Bar>& into,
    std::optional<std::string>& cursor,
    Currency& currency
  ) {
    if (!result) {
      return std::move(result.error());
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return parse_error;
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return toss_error(result.value(), *document);
    }

    try {
      const auto& payload = required_object(*document, "result");
      const auto rows = payload.if_contains("candles");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: candles");
      }
      const auto before = into.size();
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("candle entry is not an object");
        }
        const auto& row = value.as_object();
        // The currency belongs to the series, so it is read once and then checked:
        // a page that changed currency mid-series would not be one series.
        const auto stated = Currency(broker_detail::json_string(row, "currency"));
        if (into.empty() && before == 0) {
          currency = stated;
        } else if (!(stated == currency)) {
          throw std::runtime_error("candle currency changed within one series");
        }
        into.push_back(parse_bar(row, query));
      }
      if (into.size() == before) {
        cursor.reset();
        return std::nullopt;
      }
      if (const auto next = payload.if_contains("nextBefore"); next && next->is_string()) {
        cursor = std::string(next->as_string());
      } else {
        cursor.reset();
      }
      return std::nullopt;
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_candle_response";
      error.message_ = exception.what();
      return error;
    }
  }

  /**
   * Normalizes one Toss candle.
   *
   * Toss stamps a one-minute bar at the instant it closed, aggregating
   * `[t - 1m, t)`, and a one-day bar at local midnight, which is its open. The
   * convention travels in the bar, because reading one as the other is a lookahead
   * no timestamp can reveal.
   */
  static Bar parse_bar(const json::object& row, const BarQuery& query) {
    static_cast<void>(query);
    Bar bar;
    const auto timestamp = parse_iso8601_timestamp(broker_detail::json_string(row, "timestamp"));
    if (!timestamp) {
      throw std::runtime_error("invalid candle timestamp");
    }
    bar.timestamp_ = *timestamp;
    bar.open_ = Decimal64::parse(broker_detail::json_string(row, "openPrice"));
    bar.high_ = Decimal64::parse(broker_detail::json_string(row, "highPrice"));
    bar.low_ = Decimal64::parse(broker_detail::json_string(row, "lowPrice"));
    bar.close_ = Decimal64::parse(broker_detail::json_string(row, "closePrice"));
    bar.volume_ = Decimal64::parse(broker_detail::json_string(row, "volume"));
    return bar;
  }

  /**
   * Reverses the accumulation and trims to the requested count.
   *
   * Paging runs backwards from the newest bar, so an overshoot is always at the
   * old end; the newest `count_` bars are what the caller asked for.
   */
  void finish_bars(
    const std::shared_ptr<BarOperation>& operation,
    const std::optional<std::string>& cursor
  ) {
    BarSeries series;
    series.instrument_ = operation->query_.instrument_;
    series.interval_ = operation->query_.interval_;
    // Toss stamps a one-minute bar at the instant it closed, aggregating
    // `[t - 1m, t)`, and a one-day bar at local midnight, which is its open.
    series.stamp_ = operation->query_.interval_ == BarInterval::ONE_MINUTE ?
      BarStamp::CLOSE :
      BarStamp::OPEN;
    series.currency_ = operation->currency_;
    series.adjusted_ = operation->query_.adjusted_;
    series.received_at_ = std::chrono::system_clock::now();

    auto& bars = operation->descending_;
    if (static_cast<int>(bars.size()) > operation->query_.count_) {
      bars.resize(static_cast<std::size_t>(operation->query_.count_));
    }
    series.bars_.assign(bars.rbegin(), bars.rend());
    if (cursor && !cursor->empty()) {
      series.next_cursor_ = *cursor;
    }
    deliver(std::move(operation->completion_), Result<BarSeries>(std::move(series)));
  }

  /**
   * Returns the `result` member of a market-data response, or the error ending it.
   *
   * Toss wraps every market-data answer in `result`, sometimes as an object and
   * sometimes as a bare array, so the shape check belongs to each parser while the
   * status, envelope, and JSON handling belong here once.
   */
  static Result<json::value> market_result(Result<network::HttpResponse> response) {
    if (!response) {
      return Result<json::value>(std::move(response.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(response.value().body(), parse_error);
    if (!document) {
      return Result<json::value>(std::move(parse_error));
    }
    if (response.value().result_int() < 200 || response.value().result_int() >= 300) {
      return Result<json::value>(toss_error(response.value(), *document));
    }
    const auto payload = document->if_contains("result");
    if (!payload) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_market_response";
      error.message_ = "response carries no result";
      return Result<json::value>(std::move(error));
    }
    return Result<json::value>(*payload);
  }

  /**
   * Normalizes Toss's reference data for one instrument.
   *
   * The identity comes from the instrument the caller asked about rather than from
   * the response's symbol, so the country and venue the caller stated survive.
   *
   * `price_rule_` is the library's own grid, not Toss's: no broker publishes tick
   * size, so the rule is chosen from the market rather than normalized from a field.
   */
  static Result<Instrument> parse_instrument(
    InstrumentId requested,
    Result<network::HttpResponse> result
  ) {
    auto payload = market_result(std::move(result));
    if (!payload) {
      return Result<Instrument>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_array() || payload.value().as_array().empty()) {
        throw std::runtime_error("reference data result is not a non-empty array");
      }
      const auto& row = payload.value().as_array().front().as_object();
      if (broker_detail::json_string(row, "symbol") != requested.symbol_) {
        throw std::runtime_error("reference data response symbol does not match request");
      }

      Instrument instrument;
      instrument.id_ = std::move(requested);
      instrument.name_ = broker_detail::json_string(row, "name");
      instrument.english_name_ = broker_detail::json_string(row, "englishName");
      instrument.isin_ = broker_detail::json_string(row, "isinCode");
      instrument.segment_ = market_segment(broker_detail::json_string(row, "market"));
      instrument.type_ = normalized_security_type(broker_detail::json_string(row, "securityType"));
      instrument.status_ =
        normalized_listing_status(broker_detail::json_string(row, "status"));
      instrument.currency_ = Currency(broker_detail::json_string(row, "currency"));
      if (const auto value = row.if_contains("isCommonShare"); value && value->is_bool()) {
        instrument.common_share_ = value->as_bool();
      }
      instrument.listed_on_ = optional_calendar_date(row, "listDate");
      instrument.delisted_on_ = optional_calendar_date(row, "delistDate");
      instrument.shares_outstanding_ = nullable_decimal(row, "sharesOutstanding");
      instrument.price_rule_ =
        price_rule_for(instrument.id_.country_, instrument.type_);
      return Result<Instrument>(std::move(instrument));
    } catch (const std::exception& exception) {
      return Result<Instrument>(market_error("invalid_reference_response", exception));
    }
  }

  /** Maps Toss's security type, tolerating a value this library has not normalized. */
  static SecurityType normalized_security_type(std::string_view code) {
    if (code == "STOCK") {
      return SecurityType::STOCK;
    }
    if (code == "FOREIGN_STOCK") {
      return SecurityType::FOREIGN_STOCK;
    }
    if (code == "DEPOSITARY_RECEIPT") {
      return SecurityType::DEPOSITARY_RECEIPT;
    }
    if (code == "INFRASTRUCTURE_FUND") {
      return SecurityType::INFRASTRUCTURE_FUND;
    }
    if (code == "REIT") {
      return SecurityType::REIT;
    }
    if (code == "ETF") {
      return SecurityType::ETF;
    }
    if (code == "FOREIGN_ETF") {
      return SecurityType::FOREIGN_ETF;
    }
    if (code == "ETN") {
      return SecurityType::ETN;
    }
    if (code == "STOCK_WARRANTS") {
      return SecurityType::STOCK_WARRANT;
    }
    return SecurityType::UNKNOWN;
  }

  /**
   * Maps Toss's listing status, tolerating an unfamiliar value.
   *
   * Named apart from the order-status mapper on purpose: a listing status and an
   * order status are unrelated vocabularies that happen to share a broker field
   * name, and overloading on return type alone would not compile anyway.
   */
  static InstrumentStatus normalized_listing_status(std::string_view code) {
    if (code == "SCHEDULED") {
      return InstrumentStatus::SCHEDULED;
    }
    if (code == "ACTIVE") {
      return InstrumentStatus::ACTIVE;
    }
    if (code == "DELISTED") {
      return InstrumentStatus::DELISTED;
    }
    return InstrumentStatus::UNKNOWN;
  }

  /** Reads a `YYYY-MM-DD` field that Toss may report as null. */
  static std::optional<Date> optional_calendar_date(
    const json::object& row,
    std::string_view key
  ) {
    const auto value = row.if_contains(key);
    if (!value || !value->is_string()) {
      return std::nullopt;
    }
    return parse_calendar_date(value->as_string().c_str());
  }

  /** Validates the request and asks for the calendar of one market. */
  void start_calendar(Country country, std::optional<Date> from, CalendarHandler completion) {
    if (from && !is_valid_date(*from)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_date";
      error.message_ = "calendar request requires a valid Gregorian date";
      deliver(std::move(completion), Result<TradingCalendar>(std::move(error)));
      return;
    }

    ensure_token(
      TokenHandler(
        [self = shared_from_this(), country, from, completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->deliver(
              std::move(completion), Result<TradingCalendar>(std::move(*token_error))
            );
            return;
          }
          network::HttpRequest request;
          // The calendar is reference data, the lowest-priority traffic there is.
          request.group_ = network::RateLimitGroup::REFERENCE;
          request.method_ = http::verb::get;
          request.target_ = std::string("/api/v1/market-calendar/") +
            (country == Country::US ? "US" : "KR");
          if (from) {
            request.target_ += "?date=" + iso_date(*from);
          }
          request.headers_.emplace_back(
            "Authorization", "Bearer " + self->token_.access_token_
          );
          self->http_->async_request(
            std::move(request),
            [self, country, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse_calendar(country, std::move(result))
              );
            }
          );
        }
      )
    );
  }

  /**
   * Normalizes Toss's calendar into the three days it reports.
   *
   * A day is a trading day when Toss reports trading hours for it: it documents the
   * hours as null "둘 다 휴장이면", when both KRX and NXT are closed. So the presence of
   * the hours *is* the answer, and the hours themselves are not normalized — only
   * Toss publishes them, so they stay reachable on this client rather than becoming
   * a field permanently empty for KIS.
   *
   * The two days either side are reported as business days, which is why they are
   * recorded as open without consulting their hours: Toss has already answered the
   * question by calling them that.
   */
  static Result<TradingCalendar> parse_calendar(
    Country country,
    Result<network::HttpResponse> result
  ) {
    auto payload = market_result(std::move(result));
    if (!payload) {
      return Result<TradingCalendar>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_object()) {
        throw std::runtime_error("calendar result is not an object");
      }
      const auto& object = payload.value().as_object();
      TradingCalendar calendar;
      calendar.country_ = country;
      append_calendar_day(object, "previousBusinessDay", true, calendar);
      append_calendar_day(object, "today", false, calendar);
      append_calendar_day(object, "nextBusinessDay", true, calendar);
      std::sort(
        calendar.days_.begin(),
        calendar.days_.end(),
        [](const TradingDay& lhs, const TradingDay& rhs) { return lhs.date_ < rhs.date_; }
      );
      calendar.received_at_ = std::chrono::system_clock::now();
      return Result<TradingCalendar>(std::move(calendar));
    } catch (const std::exception& exception) {
      return Result<TradingCalendar>(market_error("invalid_calendar_response", exception));
    }
  }

  /** Reads one reported day, deciding whether the market opens on it. */
  static void append_calendar_day(
    const json::object& object,
    std::string_view key,
    bool business_day,
    TradingCalendar& calendar
  ) {
    const auto value = object.if_contains(key);
    if (!value || !value->is_object()) {
      return;
    }
    const auto& row = value->as_object();
    const auto date = parse_calendar_date(broker_detail::json_string(row, "date"));
    if (!date) {
      throw std::runtime_error("invalid calendar date");
    }
    TradingDay day;
    day.date_ = *date;
    if (business_day) {
      day.open_ = true;
    } else {
      const auto hours = row.if_contains("integrated");
      day.open_ = hours && !hours->is_null();
    }
    calendar.days_.push_back(day);
  }

  /** Parses a `YYYY-MM-DD` calendar date. */
  static std::optional<Date> parse_calendar_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
      return std::nullopt;
    }
    Date date;
    date.year_ = std::stoi(std::string(text.substr(0, 4)));
    date.month_ = static_cast<unsigned>(std::stoi(std::string(text.substr(5, 2))));
    date.day_ = static_cast<unsigned>(std::stoi(std::string(text.substr(8, 2))));
    return is_valid_date(date) ? std::optional<Date>(date) : std::nullopt;
  }

  /** Most executions Toss returns in one request. */
  static constexpr int trade_limit_ = 50;

  /** Most instruments Toss quotes in one request. */
  static constexpr int price_batch_limit_ = 200;

  /**
   * Issues one authenticated market-data GET for a single instrument.
   *
   * The three single-instrument reads differ only in their path and their parser,
   * so the token handling, the traffic class, and the validation live here once.
   */
  template <typename Handler, typename Parse>
  void start_market_read(
    InstrumentId instrument,
    std::string prefix,
    Handler completion,
    Parse parse
  ) {
    using ResultType = decltype(parse(InstrumentId{}, Result<network::HttpResponse>(BrokerError{})));
    static_assert(!std::is_void<ResultType>::value, "a market-data parser must return a Result");
    if (!broker_detail::valid_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "market data request requires a symbol";
      deliver(std::move(completion), ResultType(std::move(error)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          instrument = std::move(instrument),
          prefix = std::move(prefix),
          completion = std::move(completion),
          parse
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(std::move(completion), ResultType(std::move(*token_error)));
            return;
          }
          network::HttpRequest request;
          // Market data is the traffic the trading reserve holds back.
          request.group_ = network::RateLimitGroup::MARKET_DATA;
          request.method_ = http::verb::get;
          request.target_ = prefix + broker_detail::query_encode(instrument.symbol_);
          request.headers_.emplace_back(
            "Authorization", "Bearer " + self->token_.access_token_
          );
          self->http_->async_request(
            std::move(request),
            [self, instrument = std::move(instrument), completion = std::move(completion), parse](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse(std::move(instrument), std::move(result))
              );
            }
          );
        }
      )
    );
  }

  /**
   * Normalizes Toss's price bounds.
   *
   * Both bounds are null in a market without a price limit, and the absence is
   * preserved: a bound of zero would read as "this instrument may not trade above
   * nothing", which is not what an unlimited market means.
   */
  static Result<PriceLimits> parse_price_limits(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    static_cast<void>(instrument);
    auto payload = market_result(std::move(result));
    if (!payload) {
      return Result<PriceLimits>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_object()) {
        throw std::runtime_error("price limit result is not an object");
      }
      const auto& object = payload.value().as_object();
      PriceLimits limits;
      limits.currency_ = Currency(broker_detail::json_string(object, "currency"));
      limits.upper_ = nullable_decimal(object, "upperLimitPrice");
      limits.lower_ = nullable_decimal(object, "lowerLimitPrice");
      if (const auto stamp = object.if_contains("timestamp"); stamp && stamp->is_string()) {
        limits.exchange_time_ = parse_iso8601_timestamp(stamp->as_string().c_str());
      }
      return Result<PriceLimits>(std::move(limits));
    } catch (const std::exception& exception) {
      return Result<PriceLimits>(market_error("invalid_price_limit_response", exception));
    }
  }

  /** Normalizes Toss's recent executions. */
  static Result<std::vector<TradePrint>> parse_trades(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    static_cast<void>(instrument);
    auto payload = market_result(std::move(result));
    if (!payload) {
      return Result<std::vector<TradePrint>>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_array()) {
        throw std::runtime_error("trade result is not an array");
      }
      const auto& rows = payload.value().as_array();
      std::vector<TradePrint> prints;
      prints.reserve(rows.size());
      for (const auto& value : rows) {
        if (!value.is_object()) {
          throw std::runtime_error("trade entry is not an object");
        }
        const auto& row = value.as_object();
        TradePrint print;
        print.price_ = Decimal64::parse(broker_detail::json_string(row, "price"));
        print.quantity_ = Decimal64::parse(broker_detail::json_string(row, "volume"));
        print.currency_ = Currency(broker_detail::json_string(row, "currency"));
        print.exchange_time_ =
          parse_iso8601_timestamp(broker_detail::json_string(row, "timestamp"));
        if (!print.exchange_time_) {
          throw std::runtime_error("invalid trade timestamp");
        }
        prints.push_back(std::move(print));
      }
      return Result<std::vector<TradePrint>>(std::move(prints));
    } catch (const std::exception& exception) {
      return Result<std::vector<TradePrint>>(market_error("invalid_trade_response", exception));
    }
  }

  /** Validates a batch and requests every instrument's price in one call. */
  void start_prices(std::vector<InstrumentId> instruments, PricesHandler completion) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (instruments.empty()) {
      error.broker_code_ = "empty_batch";
      error.message_ = "a batch price request needs at least one instrument";
      deliver(std::move(completion), Result<std::vector<QuoteSnapshot>>(std::move(error)));
      return;
    }
    if (static_cast<int>(instruments.size()) > price_batch_limit_) {
      error.broker_code_ = "batch_too_large";
      error.message_ =
        "Toss quotes at most two hundred instruments per request; a longer list is "
        "refused rather than split, because splitting would spend several times the "
        "request budget a batch call exists to save";
      deliver(std::move(completion), Result<std::vector<QuoteSnapshot>>(std::move(error)));
      return;
    }

    std::string symbols;
    for (const auto& instrument : instruments) {
      if (!broker_detail::valid_symbol(instrument.symbol_)) {
        error.broker_code_ = "invalid_instrument";
        error.message_ = "batch price request requires a symbol for every instrument";
        deliver(std::move(completion), Result<std::vector<QuoteSnapshot>>(std::move(error)));
        return;
      }
      if (!symbols.empty()) {
        symbols += ',';
      }
      symbols += instrument.symbol_;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          instruments = std::move(instruments),
          symbols = std::move(symbols),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(completion),
              Result<std::vector<QuoteSnapshot>>(std::move(*token_error))
            );
            return;
          }
          network::HttpRequest request;
          request.group_ = network::RateLimitGroup::MARKET_DATA;
          request.method_ = http::verb::get;
          request.target_ = "/api/v1/prices?symbols=" + broker_detail::query_encode(symbols);
          request.headers_.emplace_back(
            "Authorization", "Bearer " + self->token_.access_token_
          );
          self->http_->async_request(
            std::move(request),
            [self, instruments = std::move(instruments), completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse_prices(instruments, std::move(result))
              );
            }
          );
        }
      )
    );
  }

  /**
   * Normalizes a batch of quotes, keeping the caller's instrument identities.
   *
   * Toss answers with a symbol per row, so each row is matched back to the
   * instrument the caller asked about rather than being rebuilt from the symbol
   * alone, which would lose the country and venue the caller stated. A row for a
   * symbol that was not asked for is a protocol error.
   */
  static Result<std::vector<QuoteSnapshot>> parse_prices(
    const std::vector<InstrumentId>& requested,
    Result<network::HttpResponse> result
  ) {
    auto payload = market_result(std::move(result));
    if (!payload) {
      return Result<std::vector<QuoteSnapshot>>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_array()) {
        throw std::runtime_error("price result is not an array");
      }
      const auto& rows = payload.value().as_array();
      std::vector<QuoteSnapshot> quotes;
      quotes.reserve(rows.size());
      for (const auto& value : rows) {
        if (!value.is_object()) {
          throw std::runtime_error("price entry is not an object");
        }
        const auto& row = value.as_object();
        const auto symbol = broker_detail::json_string(row, "symbol");
        const auto match = std::find_if(
          requested.begin(),
          requested.end(),
          [&symbol](const InstrumentId& candidate) { return candidate.symbol_ == symbol; }
        );
        if (match == requested.end()) {
          throw std::runtime_error("price response carries an unrequested symbol");
        }
        QuoteSnapshot quote;
        quote.instrument_ = *match;
        quote.last_price_ = Decimal64::parse(broker_detail::json_string(row, "lastPrice"));
        quote.currency_ = Currency(broker_detail::json_string(row, "currency"));
        if (const auto stamp = row.if_contains("timestamp"); stamp && stamp->is_string()) {
          quote.exchange_time_ = parse_iso8601_timestamp(stamp->as_string().c_str());
        }
        quote.received_at_ = std::chrono::system_clock::now();
        quotes.push_back(std::move(quote));
      }
      return Result<std::vector<QuoteSnapshot>>(std::move(quotes));
    } catch (const std::exception& exception) {
      return Result<std::vector<QuoteSnapshot>>(market_error("invalid_price_response", exception));
    }
  }

  /** Returns a protocol error naming the response that could not be read. */
  static BrokerError market_error(const char* code, const std::exception& cause) {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = code;
    error.message_ = cause.what();
    return error;
  }

  /** Validates the instrument and requests its book. */
  void start_depth(InstrumentId instrument, DepthHandler completion) {
    if (!broker_detail::valid_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "order book request requires a symbol";
      deliver(std::move(completion), Result<DepthSnapshot>(std::move(error)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          instrument = std::move(instrument),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(completion), Result<DepthSnapshot>(std::move(*token_error))
            );
            return;
          }
          network::HttpRequest request;
          // The book is polling traffic, held back by the trading reserve.
          request.group_ = network::RateLimitGroup::MARKET_DATA;
          request.method_ = http::verb::get;
          request.target_ = "/api/v1/orderbook?symbol=" +
            broker_detail::query_encode(instrument.symbol_);
          request.headers_.emplace_back(
            "Authorization", "Bearer " + self->token_.access_token_
          );
          self->http_->async_request(
            std::move(request),
            [self, instrument = std::move(instrument), completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse_depth(std::move(instrument), std::move(result))
              );
            }
          );
        }
      )
    );
  }

  /**
   * Normalizes one Toss order book.
   *
   * The levels are sorted rather than taken in the order they arrive. Toss's
   * specification documents `asks` as ascending and its own worked example lists
   * them descending, so trusting either would be a guess, and reading the best
   * offer off the wrong end of the book makes the spread negative.
   */
  static Result<DepthSnapshot> parse_depth(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      return Result<DepthSnapshot>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<DepthSnapshot>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<DepthSnapshot>(toss_error(result.value(), *document));
    }
    try {
      const auto& payload = required_object(*document, "result");
      DepthSnapshot book;
      book.instrument_ = std::move(instrument);
      book.currency_ = Currency(broker_detail::json_string(payload, "currency"));
      append_levels(payload, "bids", book.bids_);
      append_levels(payload, "asks", book.asks_);
      order_depth(book);
      if (const auto stamp = payload.if_contains("timestamp"); stamp && stamp->is_string()) {
        book.exchange_time_ = parse_iso8601_timestamp(stamp->as_string().c_str());
        if (!book.exchange_time_) {
          throw std::runtime_error("invalid order book timestamp");
        }
      }
      book.received_at_ = std::chrono::system_clock::now();
      return Result<DepthSnapshot>(std::move(book));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_orderbook_response";
      error.message_ = exception.what();
      return Result<DepthSnapshot>(std::move(error));
    }
  }

  /** Reads one side of the book into `levels`. */
  static void append_levels(
    const json::object& payload,
    std::string_view key,
    std::vector<DepthLevel>& levels
  ) {
    const auto rows = payload.if_contains(key);
    if (!rows || !rows->is_array()) {
      throw std::runtime_error("missing array field: " + std::string(key));
    }
    levels.reserve(rows->as_array().size());
    for (const auto& value : rows->as_array()) {
      if (!value.is_object()) {
        throw std::runtime_error("order book entry is not an object");
      }
      const auto& row = value.as_object();
      levels.push_back(
        DepthLevel{
          Decimal64::parse(broker_detail::json_string(row, "price")),
          Decimal64::parse(broker_detail::json_string(row, "volume")),
        }
      );
    }
  }

  void start_quote(InstrumentId instrument, QuoteHandler completion) {
    if (instrument.country_ != Country::KR || !broker_detail::valid_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "Toss quote currently supports Korean equity symbols only";
      complete(std::move(completion), Result<QuoteSnapshot>(std::move(error)));
      return;
    }
    ensure_token(
      TokenHandler(
        [self = shared_from_this(),
          instrument = std::move(instrument),
          completion = std::move(completion)](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete(
              std::move(completion), Result<QuoteSnapshot>(std::move(*token_error))
            );
            return;
          }
          self->request_quote(std::move(instrument), std::move(completion));
        }
      )
    );
  }

  void start_accounts(AccountsHandler completion) {
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->complete_accounts(
              std::move(completion),
              Result<std::vector<TossAccount>>(std::move(*token_error))
            );
            return;
          }
          self->request_accounts(std::move(completion));
        }
      )
    );
  }

  void start_buying_power(
    Currency currency,
    std::int64_t account_sequence,
    BuyingPowerHandler completion
  ) {
    auto invalid = validate_account_sequence(account_sequence);
    if (!invalid && !priced_currency(currency)) {
      // Refused here rather than sent: the endpoint answers for the currencies Toss prices, and a
      // request for any other is a condition the caller stated that cannot be honored.
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "unsupported_currency";
      error.message_ = "Toss prices buying power in KRW and USD only, not " + currency.str();
      invalid = std::move(error);
    }
    if (invalid) {
      complete_buying_power(std::move(completion), Result<BuyingPower>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), currency, account_sequence, completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->complete_buying_power(
              std::move(completion), Result<BuyingPower>(std::move(*token_error))
            );
            return;
          }
          self->request_buying_power(currency, account_sequence, std::move(completion));
        }
      )
    );
  }

  void request_buying_power(
    Currency currency,
    std::int64_t account_sequence,
    BuyingPowerHandler completion
  ) {
    network::HttpRequest request;
    // The same class as holdings: an account read, above polling and below trading.
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/buying-power?currency=" + currency.str();
    request.headers_ = {
      {"Authorization", "Bearer " + token_.access_token_},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
    };
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), completion = std::move(completion)](
        Result<network::HttpResponse> result
      ) mutable {
        net::post(
          self->strand_,
          [self, completion = std::move(completion), result = std::move(result)]() mutable {
            self->complete_buying_power(
              std::move(completion), parse_buying_power(std::move(result))
            );
          }
        );
      }
    );
  }

  void start_sellable_quantity(
    InstrumentId instrument,
    std::int64_t account_sequence,
    SellableQuantityHandler completion
  ) {
    if (auto invalid = validate_account_sequence(account_sequence)) {
      complete_sellable_quantity(std::move(completion), Result<Decimal64>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), instrument = std::move(instrument), account_sequence,
         completion = std::move(completion)](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_sellable_quantity(
              std::move(completion), Result<Decimal64>(std::move(*token_error))
            );
            return;
          }
          self->request_sellable_quantity(
            std::move(instrument), account_sequence, std::move(completion)
          );
        }
      )
    );
  }

  void request_sellable_quantity(
    InstrumentId instrument,
    std::int64_t account_sequence,
    SellableQuantityHandler completion
  ) {
    network::HttpRequest request;
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/sellable-quantity?symbol=" + instrument.symbol_;
    request.headers_ = {
      {"Authorization", "Bearer " + token_.access_token_},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
    };
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), completion = std::move(completion)](
        Result<network::HttpResponse> result
      ) mutable {
        net::post(
          self->strand_,
          [self, completion = std::move(completion), result = std::move(result)]() mutable {
            self->complete_sellable_quantity(
              std::move(completion), parse_sellable_quantity(std::move(result))
            );
          }
        );
      }
    );
  }

  void start_commissions(std::int64_t account_sequence, CommissionScheduleHandler completion) {
    if (auto invalid = validate_account_sequence(account_sequence)) {
      complete_commissions(
        std::move(completion), Result<CommissionSchedule>(std::move(*invalid))
      );
      return;
    }
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), account_sequence, completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->complete_commissions(
              std::move(completion), Result<CommissionSchedule>(std::move(*token_error))
            );
            return;
          }
          self->request_commissions(account_sequence, std::move(completion));
        }
      )
    );
  }

  void request_commissions(std::int64_t account_sequence, CommissionScheduleHandler completion) {
    network::HttpRequest request;
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/commissions";
    request.headers_ = {
      {"Authorization", "Bearer " + token_.access_token_},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
    };
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), completion = std::move(completion)](
        Result<network::HttpResponse> result
      ) mutable {
        net::post(
          self->strand_,
          [self, completion = std::move(completion), result = std::move(result)]() mutable {
            self->complete_commissions(
              std::move(completion), parse_commissions(std::move(result))
            );
          }
        );
      }
    );
  }

  void start_holdings(std::int64_t account_sequence, HoldingsHandler completion) {
    if (auto invalid = validate_account_sequence(account_sequence)) {
      complete_holdings(
        std::move(completion), Result<HoldingsSnapshot>(std::move(*invalid))
      );
      return;
    }
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), account_sequence, completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->complete_holdings(
              std::move(completion), Result<HoldingsSnapshot>(std::move(*token_error))
            );
            return;
          }
          self->request_holdings(account_sequence, std::move(completion));
        }
      )
    );
  }

  void ensure_token(TokenHandler handler) {
    if (token_.valid()) {
      std::move(handler)(std::nullopt);
      return;
    }
    token_waiters_.push_back(std::move(handler));
    if (refreshing_) {
      return;
    }
    refreshing_ = true;
    network::HttpRequest request;
    // Every other request waits behind a token refresh, including a cancellation.
    request.group_ = network::RateLimitGroup::AUTH;
    request.method_ = http::verb::post;
    request.target_ = "/oauth2/token";
    request.body_ = config_.to_form_urlencoded_body();
    request.headers_.emplace_back("content-type", "application/x-www-form-urlencoded");
    http_->async_request(
      std::move(request),
      [self = shared_from_this()](Result<network::HttpResponse> result) mutable {
        net::post(self->strand_, [self, result = std::move(result)]() mutable {
          self->finish_token(std::move(result));
        });
      }
    );
  }

  void finish_token(Result<network::HttpResponse> result) {
    refreshing_ = false;
    std::optional<BrokerError> failure;
    if (!result) {
      failure = std::move(result.error());
    } else {
      BrokerError parse_error;
      auto document = broker_detail::parse_object(result.value().body(), parse_error);
      if (!document) {
        failure = std::move(parse_error);
      } else if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
        failure = toss_error(result.value(), *document);
        failure->category_ = ErrorCategory::AUTHENTICATION;
      } else {
        try {
          token_.from_json(*document);
        } catch (const std::exception& exception) {
          BrokerError error;
          error.category_ = ErrorCategory::AUTHENTICATION;
          error.broker_code_ = "invalid_token_response";
          error.message_ = exception.what();
          failure = std::move(error);
        }
      }
    }
    if (failure) {
      // **The waiters asked for something else.** A token refresh that fails is handed to every
      // caller waiting behind it -- an order, a cancellation, an inquiry -- and none of those
      // requests was sent. Left unmarked, an authentication failure reads to a runner as the broker
      // refusing an order it never received: a request counted against the rate window, and a later
      // valid order refused once credentials recover. What failed was the token request, and that
      // is not the instruction this error is about to describe.
      failure->reached_broker_ = false;
    }
    auto waiters = std::move(token_waiters_);
    token_waiters_.clear();
    for (auto& waiter : waiters) {
      if (failure) {
        std::move(waiter)(*failure);
      } else {
        std::move(waiter)(std::nullopt);
      }
    }
  }

  void request_quote(InstrumentId instrument, QuoteHandler completion) {
    network::HttpRequest request;
    // Quote polling is the traffic a reserve exists to hold back.
    request.group_ = network::RateLimitGroup::MARKET_DATA;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/prices?symbols=" + instrument.symbol_;
    request.headers_.emplace_back("Authorization", "Bearer " + token_.access_token_);
    http_->async_request(
      std::move(request),
      [
        self = shared_from_this(),
        instrument = std::move(instrument),
        completion = std::move(completion)
      ](Result<network::HttpResponse> result) mutable {
        net::post(
          self->strand_,
          [
            self,
            instrument = std::move(instrument),
            completion = std::move(completion),
            result = std::move(result)
          ]() mutable {
            self->complete(
              std::move(completion), parse_quote(std::move(instrument), std::move(result))
            );
          }
        );
      }
    );
  }

  void request_accounts(AccountsHandler completion) {
    network::HttpRequest request;
    // Account discovery runs before a session is bound.
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/accounts";
    request.headers_.emplace_back("Authorization", "Bearer " + token_.access_token_);
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), completion = std::move(completion)](
        Result<network::HttpResponse> result
      ) mutable {
        net::post(
          self->strand_,
          [self, completion = std::move(completion), result = std::move(result)]() mutable {
            self->complete_accounts(
              std::move(completion), parse_accounts(std::move(result))
            );
          }
        );
      }
    );
  }

  void request_holdings(std::int64_t account_sequence, HoldingsHandler completion) {
    network::HttpRequest request;
    // Holdings rank above polling but may not spend the trading reserve.
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/holdings";
    request.headers_ = {
      {"Authorization", "Bearer " + token_.access_token_},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
    };
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), completion = std::move(completion)](
        Result<network::HttpResponse> result
      ) mutable {
        net::post(
          self->strand_,
          [self, completion = std::move(completion), result = std::move(result)]() mutable {
            self->complete_holdings(
              std::move(completion), parse_holdings(std::move(result))
            );
          }
        );
      }
    );
  }

  static Result<QuoteSnapshot> parse_quote(InstrumentId instrument, Result<network::HttpResponse> result) {
    if (!result) {
      return Result<QuoteSnapshot>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<QuoteSnapshot>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = toss_error(result.value(), *document);
      return Result<QuoteSnapshot>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (
        !payload || !payload->is_array() || payload->as_array().empty() ||
        !payload->as_array().front().is_object()
      ) {
        throw std::runtime_error("missing non-empty result array");
      }
      const auto& output = payload->as_array().front().as_object();
      if (broker_detail::json_string(output, "symbol") != instrument.symbol_) {
        throw std::runtime_error("quote response symbol does not match request");
      }
      QuoteSnapshot quote;
      quote.instrument_ = std::move(instrument);
      quote.last_price_ = Decimal64::parse(broker_detail::json_string(output, "lastPrice"));
      quote.currency_ = Currency(broker_detail::json_string(output, "currency"));
      if (
        const auto timestamp = output.if_contains("timestamp"); timestamp && timestamp->is_string()
      ) {
        quote.exchange_time_ = parse_iso8601_timestamp(timestamp->as_string().c_str());
        if (!quote.exchange_time_) {
          throw std::runtime_error("invalid timestamp");
        }
      }
      quote.received_at_ = std::chrono::system_clock::now();
      return Result<QuoteSnapshot>(std::move(quote));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_quote_response";
      error.message_ = exception.what();
      return Result<QuoteSnapshot>(std::move(error));
    }
  }

  static Result<std::vector<TossAccount>> parse_accounts(Result<network::HttpResponse> result) {
    if (!result) {
      return Result<std::vector<TossAccount>>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<std::vector<TossAccount>>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = toss_error(result.value(), *document);
      return Result<std::vector<TossAccount>>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (!payload || !payload->is_array()) {
        throw std::runtime_error("missing array field: result");
      }
      std::vector<TossAccount> accounts;
      accounts.reserve(payload->as_array().size());
      for (const auto& value : payload->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("account item is not an object");
        }
        const auto& account = value.as_object();
        const auto sequence = broker_detail::json_int64(account, "accountSeq");
        if (sequence <= 0) {
          throw std::runtime_error("accountSeq must be positive");
        }
        accounts.push_back(
          TossAccount{
            broker_detail::json_string(account, "accountNo"),
            sequence,
            broker_detail::json_string(account, "accountType"),
          }
        );
      }
      return Result<std::vector<TossAccount>>(std::move(accounts));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_accounts_response";
      error.message_ = exception.what();
      return Result<std::vector<TossAccount>>(std::move(error));
    }
  }

  /** Returns whether Toss prices buying power in `currency`. */
  static bool priced_currency(const Currency& currency) {
    return currency == Currency::krw() || currency == Currency::usd();
  }

  /**
   * Normalizes one buying-power reading.
   *
   * The currency comes from the response rather than from the request, so a broker answering in a
   * currency other than the one asked for is carried as it answered rather than relabelled.
   */
  static Result<BuyingPower> parse_buying_power(Result<network::HttpResponse> result) {
    auto payload = account_payload(result);
    if (!payload) {
      return Result<BuyingPower>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_object()) {
        throw std::runtime_error("buying power result is not an object");
      }
      const auto& output = payload.value().as_object();
      BuyingPower power;
      power.cash_ = Money{
        Decimal64::parse(broker_detail::json_string(output, "cashBuyingPower")),
        Currency(broker_detail::json_string(output, "currency")),
      };
      // Toss states no timestamp for this figure, so the honest stamp is when it arrived.
      power.received_at_ = std::chrono::system_clock::now();
      return Result<BuyingPower>(std::move(power));
    } catch (const std::exception& exception) {
      return Result<BuyingPower>(account_error("invalid_buying_power_response", exception));
    }
  }

  /** Normalizes one instrument's sellable quantity, which Toss may report fractionally. */
  static Result<Decimal64> parse_sellable_quantity(Result<network::HttpResponse> result) {
    auto payload = account_payload(result);
    if (!payload) {
      return Result<Decimal64>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_object()) {
        throw std::runtime_error("sellable quantity result is not an object");
      }
      const auto& output = payload.value().as_object();
      return Result<Decimal64>(
        Decimal64::parse(broker_detail::json_string(output, "sellableQuantity"))
      );
    } catch (const std::exception& exception) {
      return Result<Decimal64>(account_error("invalid_sellable_quantity_response", exception));
    }
  }

  /**
   * Normalizes the account's commission schedule.
   *
   * A row for a market this library does not normalize is **skipped rather than refused**: the
   * schedule is a list of what the broker charges where, and one unfamiliar market must not deny a
   * caller the rate for the market they trade. That is the same rule the holdings and order paths
   * follow for an unrecognized row.
   */
  static Result<CommissionSchedule> parse_commissions(Result<network::HttpResponse> result) {
    auto payload = account_payload(result);
    if (!payload) {
      return Result<CommissionSchedule>(std::move(payload.error()));
    }
    try {
      if (!payload.value().is_array()) {
        throw std::runtime_error("commission result is not an array");
      }
      CommissionSchedule schedule;
      schedule.rates_.reserve(payload.value().as_array().size());
      for (const auto& value : payload.value().as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("commission item is not an object");
        }
        const auto& row = value.as_object();
        const auto market = broker_detail::json_string(row, "marketCountry");
        CommissionRate rate;
        if (market == "KR") {
          rate.country_ = Country::KR;
        } else if (market == "US") {
          rate.country_ = Country::US;
        } else if (const auto preserved = market.empty() ? std::nullopt : Code<16>::try_make(market)) {
          // Toss's own specification requires a client to accept unknown values of this enum, so a
          // new market is a normal thing to be sent. Preserved rather than dropped: a caller can
          // see what the broker charges there even though nothing here can price it, and a
          // schedule that quietly omitted the row would be a successful, incomplete answer.
          //
          // The empty string is excluded before `try_make` sees it, and that exclusion is load
          // bearing: an empty code stores successfully, and an empty `other_` is how a row says it
          // names a *normalized* market -- so a row with no market at all would become a rate for
          // whatever `country_` defaults to, which is a Korean commission the broker never
          // published. Review found exactly that path.
          rate.other_ = *preserved;
        } else {
          // Not storable, so it cannot be reported under any market honestly. One such row must not
          // fail the whole schedule, which is the rule every other listing here follows -- but it
          // is counted, so a caller can see the schedule is incomplete rather than reading a
          // silently shortened list as the whole answer.
          ++schedule.dropped_;
          continue;
        }
        rate.rate_ = Decimal64::parse(broker_detail::json_string(row, "commissionRate"));
        // Null means open-ended in that direction, which is how Toss publishes a rate with no
        // scheduled change. It is deliberately not read as "today".
        rate.from_ = optional_calendar_date(row, "startDate");
        rate.until_ = optional_calendar_date(row, "endDate");
        schedule.rates_.push_back(std::move(rate));
      }
      schedule.received_at_ = std::chrono::system_clock::now();
      return Result<CommissionSchedule>(std::move(schedule));
    } catch (const std::exception& exception) {
      return Result<CommissionSchedule>(account_error("invalid_commission_response", exception));
    }
  }

  /**
   * Returns the `result` field of an account response, or the error that stopped it.
   *
   * The three account-capacity reads share one envelope, so they share one unwrapping: transport
   * failure, unparseable body, and a non-2xx status all become the error they are, and only a
   * successful response yields a payload.
   */
  static Result<json::value> account_payload(Result<network::HttpResponse>& result) {
    if (!result) {
      return Result<json::value>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<json::value>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<json::value>(toss_error(result.value(), *document));
    }
    const auto payload = document->if_contains("result");
    if (!payload) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "missing_result";
      error.message_ = "response carries no result field";
      return Result<json::value>(std::move(error));
    }
    return Result<json::value>(*payload);
  }

  /** Builds the protocol error an account-capacity parse failure reports. */
  static BrokerError account_error(std::string code, const std::exception& exception) {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = std::move(code);
    error.message_ = exception.what();
    return error;
  }

  static Result<HoldingsSnapshot> parse_holdings(Result<network::HttpResponse> result) {
    if (!result) {
      return Result<HoldingsSnapshot>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<HoldingsSnapshot>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = toss_error(result.value(), *document);
      return Result<HoldingsSnapshot>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (!payload || !payload->is_object()) {
        throw std::runtime_error("missing object field: result");
      }
      const auto& output = payload->as_object();
      const auto items = output.if_contains("items");
      if (!items || !items->is_array()) {
        throw std::runtime_error("missing array field: items");
      }

      HoldingsSnapshot snapshot;
      snapshot.positions_.reserve(items->as_array().size());
      for (const auto& value : items->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("holdings item is not an object");
        }
        snapshot.positions_.push_back(parse_position(value.as_object()));
      }
      append_totals(output, snapshot.totals_);
      snapshot.received_at_ = std::chrono::system_clock::now();
      return Result<HoldingsSnapshot>(std::move(snapshot));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_holdings_response";
      error.message_ = exception.what();
      return Result<HoldingsSnapshot>(std::move(error));
    }
  }

  static HoldingPosition parse_position(const json::object& output) {
    const auto market_country = broker_detail::json_string(output, "marketCountry");
    const auto currency = Currency(broker_detail::json_string(output, "currency"));
    Country country;
    Venue venue = Venue::UNSPECIFIED;
    if (market_country == "KR") {
      country = Country::KR;
      venue = Venue::COMBINED;
      if (!(currency == Currency::krw())) {
        throw std::runtime_error("KR holding currency is not KRW");
      }
    } else if (market_country == "US") {
      country = Country::US;
      if (!(currency == Currency::usd())) {
        throw std::runtime_error("US holding currency is not USD");
      }
    } else {
      throw std::runtime_error("unsupported marketCountry: " + market_country);
    }

    const auto& market_value = required_object(output, "marketValue");
    const auto& profit_loss = required_object(output, "profitLoss");
    HoldingPosition position;
    position.instrument_ = InstrumentId{
      country, venue, broker_detail::json_string(output, "symbol")
    };
    position.name_ = broker_detail::json_string(output, "name");
    position.quantity_ = Decimal64::parse(broker_detail::json_string(output, "quantity"));
    position.average_purchase_price_ =
      Decimal64::parse(broker_detail::json_string(output, "averagePurchasePrice"));
    position.last_price_ = Decimal64::parse(broker_detail::json_string(output, "lastPrice"));
    position.purchase_value_ = Money{
      Decimal64::parse(broker_detail::json_string(market_value, "purchaseAmount")), currency
    };
    position.market_value_ = Money{
      Decimal64::parse(broker_detail::json_string(market_value, "amount")), currency
    };
    position.unrealized_profit_loss_ = Money{
      Decimal64::parse(broker_detail::json_string(profit_loss, "amount")), currency
    };
    position.unrealized_return_ =
      Decimal64::parse(broker_detail::json_string(profit_loss, "rate"));
    return position;
  }

  static const json::object& required_object(const json::object& parent, std::string_view key) {
    const auto value = parent.if_contains(key);
    if (!value || !value->is_object()) {
      throw std::runtime_error("missing object field: " + std::string(key));
    }
    return value->as_object();
  }

  static std::optional<Decimal64> optional_decimal(const json::object& object, std::string_view key) {
    const auto value = object.if_contains(key);
    if (!value) {
      throw std::runtime_error("missing decimal field: " + std::string(key));
    }
    if (value->is_null()) {
      return std::nullopt;
    }
    if (!value->is_string()) {
      throw std::runtime_error("invalid decimal field: " + std::string(key));
    }
    return Decimal64::parse(value->as_string().c_str());
  }

  static void append_totals(const json::object& output, std::vector<HoldingsTotals>& totals) {
    const auto& purchase = required_object(output, "totalPurchaseAmount");
    const auto& market = required_object(required_object(output, "marketValue"), "amount");
    const auto& profit_loss = required_object(required_object(output, "profitLoss"), "amount");
    totals.push_back(
      HoldingsTotals{
        Currency::krw(),
        Decimal64::parse(broker_detail::json_string(purchase, "krw")),
        Decimal64::parse(broker_detail::json_string(market, "krw")),
        Decimal64::parse(broker_detail::json_string(profit_loss, "krw")),
      }
    );

    const auto usd_purchase = optional_decimal(purchase, "usd");
    const auto usd_market = optional_decimal(market, "usd");
    const auto usd_profit_loss = optional_decimal(profit_loss, "usd");
    const bool any_usd = usd_purchase || usd_market || usd_profit_loss;
    const bool all_usd = usd_purchase && usd_market && usd_profit_loss;
    if (any_usd != all_usd) {
      throw std::runtime_error("USD overview fields have inconsistent nullability");
    }
    if (all_usd) {
      totals.push_back(
        HoldingsTotals{
          Currency::usd(), *usd_purchase, *usd_market, *usd_profit_loss,
        }
      );
    }
  }

  static BrokerError toss_error(const network::HttpResponse& response, const json::object& document) {
    const int status = response.result_int();
    BrokerError result;
    result.http_status_ = status;
    result.category_ = status == 429 ? ErrorCategory::RATE_LIMIT : ErrorCategory::PROTOCOL;
    if (result.category_ == ErrorCategory::RATE_LIMIT) {
      // Both hints Toss documents on a refusal, read in the one place a refusal is
      // built so no call site can forget one of them.
      result.retry_after_ = retry_after(response);
      result.rate_limit_ = reported_rate_limit(response);
    }
    const auto error = document.if_contains("error");
    if (!error || !error->is_object()) {
      result.message_ = "Toss response has no error object";
      return result;
    }
    const auto& object = error->as_object();
    if (const auto value = object.if_contains("code"); value && value->is_string()) {
      result.broker_code_ = value->as_string().c_str();
    }
    if (const auto value = object.if_contains("requestId"); value && value->is_string()) {
      result.request_id_ = value->as_string().c_str();
    }
    if (const auto value = object.if_contains("message"); value && value->is_string()) {
      result.message_ = value->as_string().c_str();
    }
    return result;
  }

  /** Returns an error when the account sequence cannot address a Toss account. */
  static std::optional<BrokerError> validate_account_sequence(std::int64_t account_sequence) {
    if (account_sequence > 0) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_account";
    error.message_ = "Toss account sequence must be positive";
    return error;
  }

  /** Returns an error when an order identifier is empty. */
  static std::optional<BrokerError> validate_order_id(std::string_view order_id) {
    if (!order_id.empty()) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_order_reference";
    error.message_ = "Toss order identifier must not be empty";
    return error;
  }

  /**
   * Returns an error when a client order identifier cannot be used as an
   * idempotency key.
   *
   * Toss accepts at most thirty-six characters drawn from letters, digits,
   * hyphen, and underscore. A key rejected on the wire would silently remove
   * the very guarantee the caller asked for, so it is checked here first.
   */
  static std::optional<BrokerError> validate_client_order_id(std::string_view client_order_id) {
    bool valid = !client_order_id.empty() && client_order_id.size() <= 36;
    for (const char ch : client_order_id) {
      const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
      valid = valid && allowed;
    }
    if (valid) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_client_order_id";
    error.message_ =
      "Toss client order id must be 1-36 characters of letters, digits, hyphen, or underscore";
    return error;
  }

  /** Returns an error when a quantity is not a positive whole number of shares. */
  static std::optional<BrokerError> validate_quantity(const Decimal64& quantity) {
    if (quantity.coefficient() > 0 && quantity.scale() == 0) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_quantity";
    error.message_ =
      "Toss order quantity must be a positive whole number of shares; "
      "fractional quantities are not supported yet";
    return error;
  }

  /** Returns an error when a price does not match its order type. */
  static std::optional<BrokerError> validate_price(
    OrderType type,
    const std::optional<Decimal64>& price
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_price";
    if (type == OrderType::LIMIT) {
      if (!price) {
        error.message_ = "Toss limit order requires a price";
        return error;
      }
      if (price->coefficient() <= 0) {
        error.message_ = "Toss limit order price must be positive";
        return error;
      }
      return std::nullopt;
    }
    if (type == OrderType::MARKET) {
      if (price) {
        error.message_ = "Toss market order must not carry a price";
        return error;
      }
      return std::nullopt;
    }
    error.broker_code_ = "unsupported_order";
    error.message_ = "Toss orders currently support limit and market orders only";
    return error;
  }

  /**
   * Returns an error when Toss cannot express this order shape.
   *
   * Toss publishes two price bases (`LIMIT`, `MARKET`), one execution condition
   * (day-valid), and three sessions, of which two are restricted. Everything
   * else this library can name is KIS's, and is rejected here rather than
   * approximated: a fill-or-kill order downgraded to a day order is a different
   * order, and a mid-price order has no Toss spelling at all.
   *
   * The restrictions are the spec's own, checked before I/O so a caller learns
   * the rule without spending an order on it: the closing auction is US limit
   * orders only, and the opening auction is Korean orders only.
   */
  static std::optional<BrokerError> validate_order_shape(
    OrderType type,
    TimeInForce time_in_force,
    OrderSession session,
    Country country
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;

    if (type != OrderType::LIMIT && type != OrderType::MARKET) {
      error.broker_code_ = "unsupported_order_type";
      error.message_ = "Toss accepts a limit or market order only";
      return error;
    }
    if (time_in_force != TimeInForce::DAY) {
      error.broker_code_ = "unsupported_time_in_force";
      error.message_ =
        "Toss orders are day-valid; it publishes no immediate-or-cancel or "
        "fill-or-kill condition";
      return error;
    }

    error.broker_code_ = "unsupported_session";
    switch (session) {
      case OrderSession::REGULAR:
        return std::nullopt;
      case OrderSession::CLOSING_AUCTION:
        if (country == Country::US && type == OrderType::LIMIT) {
          return std::nullopt;
        }
        error.message_ =
          "Toss accepts a closing-auction order only for a US limit order";
        return error;
      case OrderSession::OPENING_AUCTION:
        if (country == Country::KR) {
          return std::nullopt;
        }
        error.message_ = "Toss accepts an opening-auction order only for a Korean order";
        return error;
      default:
        error.message_ = "Toss publishes no order code for this trading session";
        return error;
    }
  }

  /** Returns an error when the order cannot be represented as a Toss order. */
  static std::optional<BrokerError> validate_order(const OrderRequest& order) {
    if (!broker_detail::valid_symbol(order.instrument_.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "Toss order symbol contains unsupported characters";
      return error;
    }
    if (order.client_order_id_) {
      if (auto error = validate_client_order_id(*order.client_order_id_)) {
        return error;
      }
    }
    if (auto error = validate_quantity(order.quantity_)) {
      return error;
    }
    if (auto error = validate_price(order.type_, order.price_)) {
      return error;
    }
    if (
      order.instrument_.country_ == Country::KR && order.price_ && order.price_->scale() != 0
    ) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_price";
      error.message_ = "Toss Korean limit order price must be a whole number of won";
      return error;
    }
    return validate_order_shape(
      order.type_, order.time_in_force_, order.session_, order.instrument_.country_
    );
  }

  /**
   * Returns Toss's wire code for a normalized price basis.
   *
   * This used to read `type == MARKET ? "MARKET" : "LIMIT"`, which was
   * harmless while those were the only two bases and is not now: it would send
   * a mid-price or best-limit amendment to Toss as a plain limit order, which
   * is the silent downgrade Decision 6 forbids and is invisible in the
   * request. Every caller validates the basis first, so the remaining cases
   * cannot be reached from outside this class; reaching one means a new body
   * was built without a validation step, which is a defect here rather than a
   * caller's bad input.
   */
  static const char* order_type_code(OrderType type) {
    switch (type) {
      case OrderType::MARKET:
        return "MARKET";
      case OrderType::LIMIT:
        return "LIMIT";
      default:
        break;
    }
    throw std::logic_error("Toss order body built from an unvalidated price basis");
  }

  /**
   * Returns an error when Toss cannot express an amendment's price basis.
   *
   * A modification states a basis and Toss accepts two of them. The placement
   * path checks this inside `validate_order_shape`; the modification path had
   * no check at all, so the basis reached `order_type_code` unexamined.
   */
  static std::optional<BrokerError> validate_amendment_shape(OrderType type) {
    if (type == OrderType::LIMIT || type == OrderType::MARKET) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "unsupported_order_type";
    error.message_ = "Toss modifies an order to a limit or market basis only";
    return error;
  }

  /**
   * Returns Toss's `timeInForce` wire value for a normalized session.
   *
   * Toss spells the session on the field it calls `timeInForce`: its own spec
   * defines `OPG` as 국내 시가단일가 and `CLS` as the closing auction, which are
   * sessions rather than validity conditions. This library keeps the two facts
   * apart -- KIS spells both inside one `ORD_DVSN` and would otherwise have to
   * put the same fact in a different field -- so the mapping to Toss's wire
   * shape happens here, at the adapter boundary, which is where Decision 2b
   * says a vendor spelling belongs.
   */
  static const char* session_code(OrderSession session) {
    switch (session) {
      case OrderSession::CLOSING_AUCTION:
        return "CLS";
      case OrderSession::OPENING_AUCTION:
        return "OPG";
      default:
        return "DAY";
    }
  }

  /** Builds the authenticated Toss order-placement POST request. */
  static network::HttpRequest make_place_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    const OrderRequest& order
  ) {
    json::object body{
      {"symbol", order.instrument_.symbol_},
      {"side", order.side_ == OrderSide::SELL ? "SELL" : "BUY"},
      {"orderType", order_type_code(order.type_)},
      {"timeInForce", session_code(order.session_)},
      {"quantity", order.quantity_.str()},
    };
    if (order.client_order_id_) {
      body.emplace("clientOrderId", *order.client_order_id_);
    }
    if (order.price_) {
      body.emplace("price", order.price_->str());
    }
    if (order.confirm_high_value_) {
      body.emplace("confirmHighValueOrder", true);
    }

    network::HttpRequest request;
    // An order is what the reserve is held for.
    request.group_ = network::RateLimitGroup::TRADING;
    request.method_ = http::verb::post;
    request.target_ = "/api/v1/orders";
    request.body_ = json::serialize(body);
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
      {"Content-Type", "application/json"},
    };
    return request;
  }

  /** Builds the authenticated Toss order-modification POST request. */
  static network::HttpRequest make_replace_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    const TossAmendment& amendment
  ) {
    json::object body{{"orderType", order_type_code(amendment.type_)}};
    if (amendment.quantity_) {
      body.emplace("quantity", amendment.quantity_->str());
    }
    if (amendment.price_) {
      body.emplace("price", amendment.price_->str());
    }
    if (amendment.confirm_high_value_) {
      body.emplace("confirmHighValueOrder", true);
    }

    network::HttpRequest request;
    // A modification carries an order instruction and ranks with one.
    request.group_ = network::RateLimitGroup::TRADING;
    request.method_ = http::verb::post;
    request.target_ =
      "/api/v1/orders/" + broker_detail::query_encode(amendment.order_id_) + "/modify";
    request.body_ = json::serialize(body);
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
      {"Content-Type", "application/json"},
    };
    return request;
  }

  /** Builds the authenticated Toss order-cancellation POST request. */
  static network::HttpRequest make_cancel_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    std::string_view order_id
  ) {
    network::HttpRequest request;
    // A cancellation is the request that must never queue behind polling.
    request.group_ = network::RateLimitGroup::TRADING;
    request.method_ = http::verb::post;
    request.target_ = "/api/v1/orders/" + broker_detail::query_encode(order_id) + "/cancel";
    request.body_ = "{}";
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
      {"Content-Type", "application/json"},
    };
    return request;
  }

  /** Builds a Toss order-list request from the supplied filters. */
  static network::HttpRequest make_orders_request(
    std::string_view access_token,
    const TossOrderQuery& query
  ) {
    const bool open = query.scope_ == TossOrderScope::OPEN;
    std::string target = "/api/v1/orders?status=";
    target += open ? "OPEN" : "CLOSED";
    if (query.symbol_) {
      target += "&symbol=" + broker_detail::query_encode(*query.symbol_);
    }
    if (query.from_) {
      target += "&from=" + iso_date(*query.from_);
    }
    if (query.to_) {
      target += "&to=" + iso_date(*query.to_);
    }
    // Toss documents that an OPEN listing returns every pending order and
    // ignores paging, so the paging parameters are omitted rather than sent
    // and silently discarded.
    if (!open) {
      if (query.cursor_) {
        target += "&cursor=" + broker_detail::query_encode(*query.cursor_);
      }
      if (query.limit_) {
        target += "&limit=" + std::to_string(*query.limit_);
      }
    }

    network::HttpRequest request;
    // Order listings are account monitoring, not order submission.
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = std::move(target);
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(query.account_sequence_)},
    };
    return request;
  }

  /** Builds a Toss request for one order by broker identifier. */
  static network::HttpRequest make_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    std::string_view order_id
  ) {
    network::HttpRequest request;
    // A single-order inquiry is account monitoring.
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/orders/" + broker_detail::query_encode(order_id);
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
    };
    return request;
  }

  /** Validates and starts one Toss order submission. */
  void start_place_order(
    std::int64_t account_sequence,
    OrderRequest order,
    OrderAckHandler completion
  ) {
    auto invalid = validate_account_sequence(account_sequence);
    if (!invalid) {
      invalid = validate_order(order);
    }
    if (invalid) {
      complete_order_ack(std::move(completion), Result<OrderAck>(std::move(*invalid)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          account_sequence,
          order = std::move(order),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order_ack(
              std::move(completion), Result<OrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = make_place_order_request(
            account_sequence, self->token_.access_token_, order
          );
          self->request_order_ack(std::move(request), std::move(completion));
        }
      )
    );
  }

  /** Validates and starts one Toss order modification. */
  void start_replace_order(
    std::int64_t account_sequence,
    TossAmendment amendment,
    OrderAckHandler completion
  ) {
    auto invalid = validate_account_sequence(account_sequence);
    if (!invalid) {
      invalid = validate_order_id(amendment.order_id_);
    }
    if (!invalid && amendment.quantity_) {
      invalid = validate_quantity(*amendment.quantity_);
    }
    if (!invalid) {
      invalid = validate_amendment_shape(amendment.type_);
    }
    if (!invalid) {
      invalid = validate_price(amendment.type_, amendment.price_);
    }
    if (invalid) {
      complete_order_ack(std::move(completion), Result<OrderAck>(std::move(*invalid)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          account_sequence,
          amendment = std::move(amendment),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order_ack(
              std::move(completion), Result<OrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = make_replace_order_request(
            account_sequence, self->token_.access_token_, amendment
          );
          self->request_order_ack(std::move(request), std::move(completion));
        }
      )
    );
  }

  /** Validates and starts one Toss order cancellation. */
  void start_cancel_order(
    std::int64_t account_sequence,
    std::string order_id,
    OrderAckHandler completion
  ) {
    auto invalid = validate_account_sequence(account_sequence);
    if (!invalid) {
      invalid = validate_order_id(order_id);
    }
    if (invalid) {
      complete_order_ack(std::move(completion), Result<OrderAck>(std::move(*invalid)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          account_sequence,
          order_id = std::move(order_id),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order_ack(
              std::move(completion), Result<OrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = make_cancel_order_request(
            account_sequence, self->token_.access_token_, order_id
          );
          self->request_order_ack(std::move(request), std::move(completion));
        }
      )
    );
  }

  /** Sends an order-writing request and dispatches its parsed acknowledgement. */
  void request_order_ack(network::HttpRequest request, OrderAckHandler completion) {
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), completion = std::move(completion)](
        Result<network::HttpResponse> result
      ) mutable {
        net::post(
          self->strand_,
          [self, completion = std::move(completion), result = std::move(result)]() mutable {
            self->complete_order_ack(std::move(completion), parse_order_ack(std::move(result)));
          }
        );
      }
    );
  }

  /** Validates and starts one filtered Toss order listing. */
  void start_orders(TossOrderQuery query, OrderPageHandler completion) {
    if (auto invalid = validate_account_sequence(query.account_sequence_)) {
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(*invalid)));
      return;
    }
    if (query.symbol_ && !broker_detail::valid_symbol(*query.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "Toss order filter symbol contains unsupported characters";
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(error)));
      return;
    }
    if (query.limit_ && (*query.limit_ < 1 || *query.limit_ > 100)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_limit";
      error.message_ = "Toss order listing accepts a page size between 1 and 100";
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(error)));
      return;
    }
    if (
      (query.from_ && !is_valid_date(*query.from_)) ||
      (query.to_ && !is_valid_date(*query.to_)) ||
      (query.from_ && query.to_ && *query.to_ < *query.from_)
    ) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_date_range";
      error.message_ = "Toss order listing requires valid inclusive calendar bounds";
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(error)));
      return;
    }

    ensure_token(
      TokenHandler(
        [self = shared_from_this(), query = std::move(query), completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->complete_order_page(
              std::move(completion), Result<OrderPage>(std::move(*token_error))
            );
            return;
          }
          auto request = make_orders_request(self->token_.access_token_, query);
          self->http_->async_request(
            std::move(request),
            [self, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              net::post(
                self->strand_,
                [self, completion = std::move(completion), result = std::move(result)]() mutable {
                  self->complete_order_page(
                    std::move(completion), parse_orders(std::move(result))
                  );
                }
              );
            }
          );
        }
      )
    );
  }

  /** Validates and starts an inquiry for one Toss order. */
  void start_order(std::int64_t account_sequence, std::string order_id, OrderHandler completion) {
    auto invalid = validate_account_sequence(account_sequence);
    if (!invalid) {
      invalid = validate_order_id(order_id);
    }
    if (invalid) {
      complete_order(std::move(completion), Result<OrderSnapshot>(std::move(*invalid)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          account_sequence,
          order_id = std::move(order_id),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order(
              std::move(completion), Result<OrderSnapshot>(std::move(*token_error))
            );
            return;
          }
          auto request = make_order_request(
            account_sequence, self->token_.access_token_, order_id
          );
          self->http_->async_request(
            std::move(request),
            [self, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              net::post(
                self->strand_,
                [self, completion = std::move(completion), result = std::move(result)]() mutable {
                  self->complete_order(std::move(completion), parse_order(std::move(result)));
                }
              );
            }
          );
        }
      )
    );
  }

  /** Parses a Toss order-instruction response into an acknowledgement. */
  static Result<OrderAck> parse_order_ack(Result<network::HttpResponse> result) {
    if (!result) {
      return Result<OrderAck>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<OrderAck>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = toss_error(result.value(), *document);
      return Result<OrderAck>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (!payload || !payload->is_object()) {
        throw std::runtime_error("missing object field: result");
      }
      const auto& fields = payload->as_object();

      OrderAck ack;
      ack.order_id_ = broker_detail::json_string(fields, "orderId");
      if (const auto value = fields.if_contains("clientOrderId"); value && value->is_string()) {
        ack.client_order_id_ = std::string(value->as_string());
      }
      ack.accepted_at_ = std::chrono::system_clock::now();
      return Result<OrderAck>(std::move(ack));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_order_response";
      error.message_ = exception.what();
      return Result<OrderAck>(std::move(error));
    }
  }

  /** Parses one Toss order-list response and its continuation cursor. */
  static Result<OrderPage> parse_orders(Result<network::HttpResponse> result) {
    if (!result) {
      return Result<OrderPage>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<OrderPage>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = toss_error(result.value(), *document);
      return Result<OrderPage>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (!payload || !payload->is_object()) {
        throw std::runtime_error("missing object field: result");
      }
      const auto& output = payload->as_object();
      const auto orders = output.if_contains("orders");
      if (!orders || !orders->is_array()) {
        throw std::runtime_error("missing array field: orders");
      }

      OrderPage page;
      page.orders_.reserve(orders->as_array().size());
      for (const auto& value : orders->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("order item is not an object");
        }
        page.orders_.push_back(parse_order_object(value.as_object()));
      }
      if (const auto cursor = output.if_contains("nextCursor"); cursor && cursor->is_string()) {
        page.next_cursor_ = std::string(cursor->as_string());
      }
      page.received_at_ = std::chrono::system_clock::now();
      return Result<OrderPage>(std::move(page));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_orders_response";
      error.message_ = exception.what();
      return Result<OrderPage>(std::move(error));
    }
  }

  /** Parses a Toss single-order response into a normalized snapshot. */
  static Result<OrderSnapshot> parse_order(Result<network::HttpResponse> result) {
    if (!result) {
      return Result<OrderSnapshot>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<OrderSnapshot>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = toss_error(result.value(), *document);
      return Result<OrderSnapshot>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (!payload || !payload->is_object()) {
        throw std::runtime_error("missing object field: result");
      }
      return Result<OrderSnapshot>(parse_order_object(payload->as_object()));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_order_response";
      error.message_ = exception.what();
      return Result<OrderSnapshot>(std::move(error));
    }
  }

  /**
   * Normalizes one Toss order object.
   *
   * Toss reports a currency rather than a market country, so the instrument's
   * country is taken from that currency. This is a stated broker fact, not an
   * inference from the symbol's shape.
   *
   * Unrecognized order-type, validity, and status codes become their `UNKNOWN`
   * value. Toss's specification requires clients to tolerate them, so one
   * unfamiliar order must not fail an entire listing.
   */
  static OrderSnapshot parse_order_object(const json::object& row) {
    const auto currency = Currency(broker_detail::json_string(row, "currency"));

    Country country;
    if (currency == Currency::krw()) {
      country = Country::KR;
    } else if (currency == Currency::usd()) {
      country = Country::US;
    } else {
      throw std::runtime_error("unsupported Toss order currency: " + currency.str());
    }

    OrderSnapshot order;
    order.order_id_ = broker_detail::json_string(row, "orderId");
    order.instrument_ = InstrumentId{
      country,
      Venue::UNSPECIFIED,
      broker_detail::json_string(row, "symbol"),
    };
    order.side_ = normalized_order_side(broker_detail::json_string(row, "side"));
    order.type_ = normalized_order_type(broker_detail::json_string(row, "orderType"));
    const auto time_in_force = broker_detail::json_string(row, "timeInForce");
    order.session_ = normalized_session(time_in_force);
    // Toss publishes no execution condition of its own, so every order it
    // recognizes is day-valid. A `timeInForce` it does not recognize leaves the
    // condition unknown as well as the session, rather than asserting `DAY`
    // about a value this library could not read.
    order.time_in_force_ =
      order.session_ == OrderSession::UNKNOWN ? TimeInForce::UNKNOWN : TimeInForce::DAY;
    if (const auto code = Code<8>::try_make(time_in_force)) {
      order.shape_code_ = *code;
    }
    order.status_ = normalized_status(broker_detail::json_string(row, "status"));
    order.price_ = nullable_decimal(row, "price");
    order.quantity_ = Decimal64::parse(broker_detail::json_string(row, "quantity"));
    // An amount-based order states dollars and lets the fill decide the
    // quantity. The library does not place one yet, but it must not drop the
    // field off one placed through another channel: without it the snapshot
    // says an order of some quantity and cannot say what was actually
    // committed. See `plan/todo/us-and-conditional-orders.md`.
    order.order_amount_ = nullable_decimal(row, "orderAmount");
    order.currency_ = currency;
    order.ordered_at_ = optional_timestamp(row, "orderedAt");
    order.canceled_at_ = optional_timestamp(row, "canceledAt");

    const auto execution = row.if_contains("execution");
    if (!execution || !execution->is_object()) {
      throw std::runtime_error("missing object field: execution");
    }
    const auto& fills = execution->as_object();
    order.execution_.filled_quantity_ =
      Decimal64::parse(broker_detail::json_string(fills, "filledQuantity"));
    order.execution_.average_filled_price_ = optional_decimal(fills, "averageFilledPrice");
    order.execution_.filled_amount_ = optional_money(fills, "filledAmount", currency);
    order.execution_.commission_ = optional_money(fills, "commission", currency);
    order.execution_.tax_ = optional_money(fills, "tax", currency);
    order.execution_.filled_at_ = optional_timestamp(fills, "filledAt");
    order.execution_.settlement_date_ = optional_date(fills, "settlementDate");
    return order;
  }

  /** Maps a Toss order-type code onto a normalized order type. */
  static OrderType normalized_order_type(std::string_view code) {
    if (code == "LIMIT") {
      return OrderType::LIMIT;
    }
    if (code == "MARKET") {
      return OrderType::MARKET;
    }
    return OrderType::UNKNOWN;
  }

  /** Maps a Toss validity code onto a normalized validity condition. */
  static OrderSession normalized_session(std::string_view code) {
    if (code == "DAY") {
      return OrderSession::REGULAR;
    }
    if (code == "CLS") {
      return OrderSession::CLOSING_AUCTION;
    }
    if (code == "OPG") {
      return OrderSession::OPENING_AUCTION;
    }
    return OrderSession::UNKNOWN;
  }

  /** Maps a Toss status code onto a normalized lifecycle state. */
  static OrderStatus normalized_status(std::string_view code) {
    if (code == "PENDING") {
      return OrderStatus::PENDING;
    }
    if (code == "PENDING_CANCEL") {
      return OrderStatus::PENDING_CANCEL;
    }
    if (code == "PENDING_REPLACE") {
      return OrderStatus::PENDING_REPLACE;
    }
    if (code == "PARTIAL_FILLED") {
      return OrderStatus::PARTIAL_FILLED;
    }
    if (code == "FILLED") {
      return OrderStatus::FILLED;
    }
    if (code == "CANCELED") {
      return OrderStatus::CANCELED;
    }
    if (code == "REJECTED") {
      return OrderStatus::REJECTED;
    }
    if (code == "CANCEL_REJECTED") {
      return OrderStatus::CANCEL_REJECTED;
    }
    if (code == "REPLACE_REJECTED") {
      return OrderStatus::REPLACE_REJECTED;
    }
    if (code == "REPLACED") {
      return OrderStatus::REPLACED;
    }
    return OrderStatus::UNKNOWN;
  }

  /** Maps a Toss side code or rejects an invalid broker response. */
  static OrderSide normalized_order_side(std::string_view code) {
    if (code == "BUY") {
      return OrderSide::BUY;
    }
    if (code == "SELL") {
      return OrderSide::SELL;
    }
    throw std::runtime_error("invalid Toss order side code");
  }

  /**
   * Reads a decimal string field that Toss may omit entirely.
   *
   * Toss marks an order's price as neither required nor always present, unlike
   * the execution amounts, which are always sent and merely nullable. Those
   * keep the stricter `optional_decimal`, so a genuinely missing execution
   * field is still reported as a protocol error.
   */
  static std::optional<Decimal64> nullable_decimal(
    const json::object& object,
    std::string_view key
  ) {
    const auto value = object.if_contains(key);
    if (!value || value->is_null()) {
      return std::nullopt;
    }
    if (!value->is_string()) {
      throw std::runtime_error("invalid decimal field: " + std::string(key));
    }
    return Decimal64::parse(std::string_view(value->as_string()));
  }

  /** Reads a nullable decimal string field as an amount in `currency`. */
  static std::optional<Money> optional_money(
    const json::object& object,
    std::string_view key,
    const Currency& currency
  ) {
    const auto amount = optional_decimal(object, key);
    if (!amount) {
      return std::nullopt;
    }
    return Money{*amount, currency};
  }

  /** Reads a nullable ISO 8601 timestamp field. */
  static std::optional<std::chrono::system_clock::time_point> optional_timestamp(
    const json::object& object,
    std::string_view key
  ) {
    const auto value = object.if_contains(key);
    if (!value || value->is_null()) {
      return std::nullopt;
    }
    if (!value->is_string()) {
      throw std::runtime_error("invalid timestamp field: " + std::string(key));
    }
    auto parsed = parse_iso8601_timestamp(std::string_view(value->as_string()));
    if (!parsed) {
      throw std::runtime_error("invalid timestamp field: " + std::string(key));
    }
    return parsed;
  }

  /** Reads a nullable `YYYY-MM-DD` calendar date field. */
  static std::optional<Date> optional_date(const json::object& object, std::string_view key) {
    const auto value = object.if_contains(key);
    if (!value || value->is_null()) {
      return std::nullopt;
    }
    if (!value->is_string()) {
      throw std::runtime_error("invalid date field: " + std::string(key));
    }
    const std::string_view text(value->as_string());
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (
      text.size() != 10 || text[4] != '-' || text[7] != '-' ||
      std::sscanf(std::string(text).c_str(), "%4d-%2u-%2u", &year, &month, &day) != 3
    ) {
      throw std::runtime_error("invalid date field: " + std::string(key));
    }
    Date date{year, month, day};
    if (!is_valid_date(date)) {
      throw std::runtime_error("invalid date field: " + std::string(key));
    }
    return date;
  }

  /** Formats a calendar date as Toss's `YYYY-MM-DD` filter bound. */
  static std::string iso_date(const Date& date) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", date.year_, date.month_, date.day_);
    return std::string(buffer);
  }

  /** Dispatches an order acknowledgement through its associated executor. */
  /**
   * Dispatches one result through its associated executor, exactly once.
   *
   * Every operation's result passes through here, which makes it the one place a
   * broker's own rate-limit refusal can be fed back into this connection's pacer
   * without each operation having to remember to do it.
   */
  template <typename Handler, typename T>
  void deliver(Handler handler, Result<T> result) {
    if (!result) {
      note_rate_limit(result.error());
    }
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  /**
   * Corrects this connection's pacing from a Toss rate-limit refusal.
   *
   * Toss documents a token bucket and reports `X-RateLimit-Limit` with its
   * refusal, so the allowance it is enforcing is known and is applied directly.
   * The headers are documented on the refusal rather than promised on every
   * response, so an absent one falls back to halving rather than to a guess.
   */
  void note_rate_limit(const BrokerError& error) {
    if (error.category_ != ErrorCategory::RATE_LIMIT) {
      return;
    }
    auto* limiter = http_->rate_limits();
    if (!limiter) {
      return;
    }
    const auto retry = error.retry_after_ ?
      std::chrono::duration_cast<network::RateLimitController::Duration>(*error.retry_after_) :
      std::chrono::duration_cast<network::RateLimitController::Duration>(std::chrono::seconds(1));
    if (error.rate_limit_) {
      limiter->shrink(*error.rate_limit_, *error.rate_limit_, retry);
      return;
    }

    // The headers are documented on the refusal rather than promised on it, so an
    // absent allowance means waiting rather than guessing a smaller one.
    limiter->hold(retry);
  }

  void complete_order_ack(OrderAckHandler handler, Result<OrderAck> result) {
    deliver(std::move(handler), std::move(result));
  }

  /** Dispatches an order-list result through its associated executor. */
  void complete_order_page(OrderPageHandler handler, Result<OrderPage> result) {
    deliver(std::move(handler), std::move(result));
  }

  /** Dispatches a single-order result through its associated executor. */
  void complete_order(OrderHandler handler, Result<OrderSnapshot> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete(QuoteHandler handler, Result<QuoteSnapshot> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete_accounts(AccountsHandler handler, Result<std::vector<TossAccount>> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete_holdings(HoldingsHandler handler, Result<HoldingsSnapshot> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete_buying_power(BuyingPowerHandler handler, Result<BuyingPower> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete_sellable_quantity(SellableQuantityHandler handler, Result<Decimal64> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete_commissions(CommissionScheduleHandler handler, Result<CommissionSchedule> result) {
    deliver(std::move(handler), std::move(result));
  }
};

/** Blocking facade that drives `TossClient` on a private event loop. */
class TossBlockingClient {
public:
  /** Constructs a blocking Toss Securities client. */
  TossBlockingClient(
    ssl::context& context,
    TossConfig config,
    network::HttpClientOptions options = {}
  )
  : client_(
      std::make_shared<TossClient>(ioc_.get_executor(), context, std::move(config), options)
    ) {}

  /** Returns the current normalized quote or a structured error. */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    return run<QuoteSnapshot>(
      [&](auto&& sink) { client_->async_quote(std::move(instrument), std::move(sink)); },
      "quote"
    );
  }

  /** Discovers the brokerage accounts available to the current credentials. */
  Result<std::vector<TossAccount>> accounts() {
    return run<std::vector<TossAccount>>(
      [&](auto&& sink) { client_->async_accounts(std::move(sink)); },
      "accounts"
    );
  }

  /** Returns normalized holdings for one discovered account sequence. */
  /** Returns static reference data for one instrument. */
  Result<Instrument> instrument(InstrumentId subject) {
    return run<Instrument>(
      [&](auto&& sink) { client_->async_instrument(std::move(subject), std::move(sink)); },
      "instrument"
    );
  }

  /** Returns the trading calendar around `from` for one market. */
  Result<TradingCalendar> calendar(Country country, std::optional<Date> from = std::nullopt) {
    return run<TradingCalendar>(
      [&](auto&& sink) { client_->async_calendar(country, from, std::move(sink)); },
      "calendar"
    );
  }

  /** Returns today's upper and lower price bound. */
  Result<PriceLimits> price_limits(InstrumentId instrument) {
    return run<PriceLimits>(
      [&](auto&& sink) { client_->async_price_limits(std::move(instrument), std::move(sink)); },
      "price_limits"
    );
  }

  /** Returns recent executions for one instrument. */
  Result<std::vector<TradePrint>> trades(InstrumentId instrument, int count) {
    return run<std::vector<TradePrint>>(
      [&](auto&& sink) {
        client_->async_trades(std::move(instrument), count, std::move(sink));
      },
      "trades"
    );
  }

  /** Returns a quote for every instrument in one request. */
  Result<std::vector<QuoteSnapshot>> prices(std::vector<InstrumentId> instruments) {
    return run<std::vector<QuoteSnapshot>>(
      [&](auto&& sink) { client_->async_prices(std::move(instruments), std::move(sink)); },
      "prices"
    );
  }

  /** Returns the resting orders on both sides of the book. */
  Result<DepthSnapshot> depth(InstrumentId instrument) {
    return run<DepthSnapshot>(
      [&](auto&& sink) { client_->async_depth(std::move(instrument), std::move(sink)); },
      "depth"
    );
  }

  /** Returns bars, paging until the query's count is satisfied. */
  Result<BarSeries> bars(BarQuery query) {
    return run<BarSeries>(
      [&](auto&& sink) { client_->async_bars(std::move(query), std::move(sink)); },
      "bars"
    );
  }

  Result<HoldingsSnapshot> holdings(std::int64_t account_sequence) {
    return run<HoldingsSnapshot>(
      [&](auto&& sink) { client_->async_holdings(account_sequence, std::move(sink)); },
      "holdings"
    );
  }

  /** Returns what the account may spend in `currency`. */
  Result<BuyingPower> buying_power(Currency currency, std::int64_t account_sequence) {
    return run<BuyingPower>(
      [&](auto&& sink) {
        client_->async_buying_power(currency, account_sequence, std::move(sink));
      },
      "buying_power"
    );
  }

  /** Returns how much of `instrument` the account may sell. */
  Result<Decimal64> sellable_quantity(InstrumentId instrument, std::int64_t account_sequence) {
    return run<Decimal64>(
      [&](auto&& sink) {
        client_->async_sellable_quantity(std::move(instrument), account_sequence, std::move(sink));
      },
      "sellable_quantity"
    );
  }

  /** Returns the commission rates in force for the account. */
  Result<CommissionSchedule> commissions(std::int64_t account_sequence) {
    return run<CommissionSchedule>(
      [&](auto&& sink) { client_->async_commissions(account_sequence, std::move(sink)); },
      "commissions"
    );
  }

  /**
   * Submits a new order and returns Toss's acknowledgement.
   *
   * A `TRANSPORT` error leaves the outcome indeterminate. Supplying
   * `OrderRequest::client_order_id_` makes a deliberate resubmission safe for
   * ten minutes; without one, reconcile through `orders()` before retrying.
   */
  Result<OrderAck> place_order(std::int64_t account_sequence, OrderRequest order) {
    return run<OrderAck>(
      [&](auto&& sink) {
        client_->async_place_order(account_sequence, std::move(order), std::move(sink));
      },
      "order placement"
    );
  }

  /** Modifies the price, and for Korean orders the quantity, of an open order. */
  Result<OrderAck> replace_order(std::int64_t account_sequence, TossAmendment amendment) {
    return run<OrderAck>(
      [&](auto&& sink) {
        client_->async_replace_order(account_sequence, std::move(amendment), std::move(sink));
      },
      "order modification"
    );
  }

  /** Cancels an open order. */
  Result<OrderAck> cancel_order(std::int64_t account_sequence, std::string order_id) {
    return run<OrderAck>(
      [&](auto&& sink) {
        client_->async_cancel_order(account_sequence, std::move(order_id), std::move(sink));
      },
      "order cancellation"
    );
  }

  /** Returns one page of orders in the requested lifecycle group. */
  Result<OrderPage> orders(TossOrderQuery query) {
    return run<OrderPage>(
      [&](auto&& sink) { client_->async_orders(std::move(query), std::move(sink)); },
      "order listing"
    );
  }

  /** Returns one order in any lifecycle state. */
  Result<OrderSnapshot> order(std::int64_t account_sequence, std::string order_id) {
    return run<OrderSnapshot>(
      [&](auto&& sink) {
        client_->async_order(account_sequence, std::move(order_id), std::move(sink));
      },
      "order inquiry"
    );
  }

  /**
   * Returns this connection's pacing statistics, or null when it is unpaced.
   *
   * A blocking facade's event loop is idle between calls, so any moment between
   * them is safe to read from.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    return client_->rate_limit_stats();
  }

private:
  /** Drives one asynchronous operation to completion on the private loop. */
  template <typename T, typename Start>
  Result<T> run(Start start, std::string_view operation) {
    std::optional<Result<T>> result;
    ioc_.restart();
    start([&result](Result<T> value) { result.emplace(std::move(value)); });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      error.broker_code_ = "event_loop_stopped";
      error.message_ = "event loop stopped before " + std::string(operation) + " completion";
      return Result<T>(std::move(error));
    }
    return std::move(*result);
  }

  net::io_context ioc_;
  std::shared_ptr<TossClient> client_;
};

} // namespace algotradelib::tossinvest
