#pragma once

#include "./config.hpp"
#include "../common.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
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

/** Persistent asynchronous Toss Securities client with coalesced OAuth refresh. */
class TossClient : public std::enable_shared_from_this<TossClient> {
public:
  /** Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;
  /** Completion handler for Toss account discovery. */
  using AccountsHandler = net::any_completion_handler<void(Result<std::vector<TossAccount>>)>;

  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;

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

  void start_quote(InstrumentId instrument, QuoteHandler completion) {
    if (instrument.country_ != Country::KR || !broker_detail::valid_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
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
        failure = toss_error(*document, result.value().result_int());
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
      auto error = toss_error(*document, result.value().result_int());
      error.retry_after_ = broker_detail::retry_after(result.value());
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
      auto error = toss_error(*document, result.value().result_int());
      error.retry_after_ = broker_detail::retry_after(result.value());
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
      auto error = toss_error(*document, result.value().result_int());
      error.retry_after_ = broker_detail::retry_after(result.value());
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

  static BrokerError toss_error(const json::object& document, int status = 0) {
    BrokerError result;
    result.http_status_ = status;
    result.category_ = status == 429 ? ErrorCategory::RATE_LIMIT : ErrorCategory::PROTOCOL;
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
   * Returns an error when a validity condition is unusable for the instrument.
   *
   * Toss restricts the at-the-close condition to United States limit orders and
   * the at-the-open condition to Korean orders. Both are rejected here rather
   * than downgraded to `DAY`, because a downgrade changes when the order
   * executes.
   */
  static std::optional<BrokerError> validate_time_in_force(
    TimeInForce time_in_force,
    OrderType type,
    Country country
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "unsupported_time_in_force";
    switch (time_in_force) {
      case TimeInForce::DAY:
        return std::nullopt;
      case TimeInForce::AT_THE_CLOSE:
        if (country == Country::US && type == OrderType::LIMIT) {
          return std::nullopt;
        }
        error.message_ = "Toss accepts an at-the-close order only for a US limit order";
        return error;
      case TimeInForce::AT_THE_OPEN:
        if (country == Country::KR) {
          return std::nullopt;
        }
        error.message_ = "Toss accepts an at-the-open order only for a Korean order";
        return error;
      default:
        error.message_ = "Toss orders require a recognized validity condition";
        return error;
    }
  }

  /** Returns an error when the order cannot be represented as a Toss order. */
  static std::optional<BrokerError> validate_order(const OrderRequest& order) {
    if (!broker_detail::valid_symbol(order.instrument_.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
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
      error.broker_code_ = "invalid_price";
      error.message_ = "Toss Korean limit order price must be a whole number of won";
      return error;
    }
    return validate_time_in_force(order.time_in_force_, order.type_, order.instrument_.country_);
  }

  /** Returns Toss's wire code for a normalized order type. */
  static const char* order_type_code(OrderType type) {
    return type == OrderType::MARKET ? "MARKET" : "LIMIT";
  }

  /** Returns Toss's wire code for a normalized validity condition. */
  static const char* time_in_force_code(TimeInForce time_in_force) {
    switch (time_in_force) {
      case TimeInForce::AT_THE_CLOSE:
        return "CLS";
      case TimeInForce::AT_THE_OPEN:
        return "OPG";
      default:
        return "DAY";
    }
  }

  static network::HttpRequest make_place_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    const OrderRequest& order
  ) {
    json::object body{
      {"symbol", order.instrument_.symbol_},
      {"side", order.side_ == OrderSide::SELL ? "SELL" : "BUY"},
      {"orderType", order_type_code(order.type_)},
      {"timeInForce", time_in_force_code(order.time_in_force_)},
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

  static network::HttpRequest make_cancel_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    std::string_view order_id
  ) {
    network::HttpRequest request;
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
    request.method_ = http::verb::get;
    request.target_ = std::move(target);
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(query.account_sequence_)},
    };
    return request;
  }

  static network::HttpRequest make_order_request(
    std::int64_t account_sequence,
    std::string_view access_token,
    std::string_view order_id
  ) {
    network::HttpRequest request;
    request.method_ = http::verb::get;
    request.target_ = "/api/v1/orders/" + broker_detail::query_encode(order_id);
    request.headers_ = {
      {"Authorization", "Bearer " + std::string(access_token)},
      {"X-Tossinvest-Account", std::to_string(account_sequence)},
    };
    return request;
  }

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

  void start_orders(TossOrderQuery query, OrderPageHandler completion) {
    if (auto invalid = validate_account_sequence(query.account_sequence_)) {
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(*invalid)));
      return;
    }
    if (query.symbol_ && !broker_detail::valid_symbol(*query.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "Toss order filter symbol contains unsupported characters";
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(error)));
      return;
    }
    if (query.limit_ && (*query.limit_ < 1 || *query.limit_ > 100)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
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
      auto error = toss_error(*document, result.value().result_int());
      error.retry_after_ = broker_detail::retry_after(result.value());
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
      auto error = toss_error(*document, result.value().result_int());
      error.retry_after_ = broker_detail::retry_after(result.value());
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
      auto error = toss_error(*document, result.value().result_int());
      error.retry_after_ = broker_detail::retry_after(result.value());
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
    order.time_in_force_ =
      normalized_time_in_force(broker_detail::json_string(row, "timeInForce"));
    order.status_ = normalized_status(broker_detail::json_string(row, "status"));
    order.price_ = nullable_decimal(row, "price");
    order.quantity_ = Decimal64::parse(broker_detail::json_string(row, "quantity"));
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
  static TimeInForce normalized_time_in_force(std::string_view code) {
    if (code == "DAY") {
      return TimeInForce::DAY;
    }
    if (code == "CLS") {
      return TimeInForce::AT_THE_CLOSE;
    }
    if (code == "OPG") {
      return TimeInForce::AT_THE_OPEN;
    }
    return TimeInForce::UNKNOWN;
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

  void complete_order_ack(OrderAckHandler handler, Result<OrderAck> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_order_page(OrderPageHandler handler, Result<OrderPage> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_order(OrderHandler handler, Result<OrderSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete(QuoteHandler handler, Result<QuoteSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_accounts(AccountsHandler handler, Result<std::vector<TossAccount>> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_holdings(HoldingsHandler handler, Result<HoldingsSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
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
  Result<HoldingsSnapshot> holdings(std::int64_t account_sequence) {
    return run<HoldingsSnapshot>(
      [&](auto&& sink) { client_->async_holdings(account_sequence, std::move(sink)); },
      "holdings"
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
