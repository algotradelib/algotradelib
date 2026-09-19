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

/** Persistent asynchronous Toss Securities client with coalesced OAuth refresh. */
class TossClient : public std::enable_shared_from_this<TossClient> {
public:
  /** Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;
  /** Completion handler for Toss account discovery. */
  using AccountsHandler = net::any_completion_handler<void(Result<std::vector<TossAccount>>)>;

  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;

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
  auto async_accounts(
    CompletionToken&& token
  ) {
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
  auto async_holdings(
    std::int64_t account_sequence,
    CompletionToken&& token
  ) {
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
    if (instrument.country != Country::KR || !broker_detail::valid_symbol(instrument.symbol)) {
      BrokerError error;
      error.category = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code = "invalid_instrument";
      error.message = "Toss quote currently supports Korean equity symbols only";
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

  void start_holdings(
    std::int64_t account_sequence,
    HoldingsHandler completion
  ) {
    if (account_sequence <= 0) {
      BrokerError error;
      error.category = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code = "invalid_account";
      error.message = "Toss account sequence must be positive";
      complete_holdings(
        std::move(completion), Result<HoldingsSnapshot>(std::move(error))
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
    request.method = http::verb::post;
    request.target = "/oauth2/token";
    request.body = config_.to_form_urlencoded_body();
    request.headers.emplace_back("content-type", "application/x-www-form-urlencoded");
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
        failure->category = ErrorCategory::AUTHENTICATION;
      } else {
        try {
          token_.from_json(*document);
        } catch (const std::exception& exception) {
          BrokerError error;
          error.category = ErrorCategory::AUTHENTICATION;
          error.broker_code = "invalid_token_response";
          error.message = exception.what();
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
    request.method = http::verb::get;
    request.target = "/api/v1/prices?symbols=" + instrument.symbol;
    request.headers.emplace_back("authorization", "Bearer " + token_.access_token_);
    http_->async_request(
      std::move(request),
      [self = shared_from_this(),
       instrument = std::move(instrument),
       completion = std::move(completion)](Result<network::HttpResponse> result) mutable {
        net::post(
          self->strand_,
          [self,
           instrument = std::move(instrument),
           completion = std::move(completion),
           result = std::move(result)]() mutable {
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
    request.method = http::verb::get;
    request.target = "/api/v1/accounts";
    request.headers.emplace_back("authorization", "Bearer " + token_.access_token_);
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

  void request_holdings(
    std::int64_t account_sequence,
    HoldingsHandler completion
  ) {
    network::HttpRequest request;
    request.method = http::verb::get;
    request.target = "/api/v1/holdings";
    request.headers = {
      {"authorization", "Bearer " + token_.access_token_},
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
      error.retry_after = broker_detail::retry_after(result.value());
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
      if (broker_detail::json_string(output, "symbol") != instrument.symbol) {
        throw std::runtime_error("quote response symbol does not match request");
      }
      QuoteSnapshot quote;
      quote.instrument = std::move(instrument);
      quote.last_price = Decimal64::parse(broker_detail::json_string(output, "lastPrice"));
      quote.currency = Currency(broker_detail::json_string(output, "currency"));
      if (
        const auto timestamp = output.if_contains("timestamp"); timestamp && timestamp->is_string()
      ) {
        quote.exchange_time = parse_iso8601_timestamp(timestamp->as_string().c_str());
        if (!quote.exchange_time) {
          throw std::runtime_error("invalid timestamp");
        }
      }
      quote.received_at = std::chrono::system_clock::now();
      return Result<QuoteSnapshot>(std::move(quote));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category = ErrorCategory::PROTOCOL;
      error.broker_code = "invalid_quote_response";
      error.message = exception.what();
      return Result<QuoteSnapshot>(std::move(error));
    }
  }

  static Result<std::vector<TossAccount>> parse_accounts(
    Result<network::HttpResponse> result
  ) {
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
      error.retry_after = broker_detail::retry_after(result.value());
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
      error.category = ErrorCategory::PROTOCOL;
      error.broker_code = "invalid_accounts_response";
      error.message = exception.what();
      return Result<std::vector<TossAccount>>(std::move(error));
    }
  }

  static Result<HoldingsSnapshot> parse_holdings(
    Result<network::HttpResponse> result
  ) {
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
      error.retry_after = broker_detail::retry_after(result.value());
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
      error.category = ErrorCategory::PROTOCOL;
      error.broker_code = "invalid_holdings_response";
      error.message = exception.what();
      return Result<HoldingsSnapshot>(std::move(error));
    }
  }

  static HoldingPosition parse_position(
    const json::object& output
  ) {
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

  static const json::object& required_object(
    const json::object& parent,
    std::string_view key
  ) {
    const auto value = parent.if_contains(key);
    if (!value || !value->is_object()) {
      throw std::runtime_error("missing object field: " + std::string(key));
    }
    return value->as_object();
  }

  static std::optional<Decimal64> optional_decimal(
    const json::object& object,
    std::string_view key
  ) {
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

  static void append_totals(
    const json::object& output,
    std::vector<HoldingsTotals>& totals
  ) {
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
    result.http_status = status;
    result.category = status == 429 ? ErrorCategory::RATE_LIMIT : ErrorCategory::PROTOCOL;
    const auto error = document.if_contains("error");
    if (!error || !error->is_object()) {
      result.message = "Toss response has no error object";
      return result;
    }
    const auto& object = error->as_object();
    if (const auto value = object.if_contains("code"); value && value->is_string()) {
      result.broker_code = value->as_string().c_str();
    }
    if (const auto value = object.if_contains("requestId"); value && value->is_string()) {
      result.request_id = value->as_string().c_str();
    }
    if (const auto value = object.if_contains("message"); value && value->is_string()) {
      result.message = value->as_string().c_str();
    }
    return result;
  }

  void complete(QuoteHandler handler, Result<QuoteSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_accounts(
    AccountsHandler handler,
    Result<std::vector<TossAccount>> result
  ) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_holdings(
    HoldingsHandler handler,
    Result<HoldingsSnapshot> result
  ) {
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
    std::optional<Result<QuoteSnapshot>> result;
    ioc_.restart();
    client_->async_quote(std::move(instrument), [&result](Result<QuoteSnapshot> value) {
      result.emplace(std::move(value));
    });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category = ErrorCategory::TRANSPORT;
      error.broker_code = "event_loop_stopped";
      error.message = "event loop stopped before quote completion";
      return Result<QuoteSnapshot>(std::move(error));
    }
    return std::move(*result);
  }

  /** Discovers the brokerage accounts available to the current credentials. */
  Result<std::vector<TossAccount>> accounts() {
    std::optional<Result<std::vector<TossAccount>>> result;
    ioc_.restart();
    client_->async_accounts([&result](Result<std::vector<TossAccount>> value) {
      result.emplace(std::move(value));
    });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category = ErrorCategory::TRANSPORT;
      error.broker_code = "event_loop_stopped";
      error.message = "event loop stopped before accounts completion";
      return Result<std::vector<TossAccount>>(std::move(error));
    }
    return std::move(*result);
  }

  /** Returns normalized holdings for one discovered account sequence. */
  Result<HoldingsSnapshot> holdings(std::int64_t account_sequence) {
    std::optional<Result<HoldingsSnapshot>> result;
    ioc_.restart();
    client_->async_holdings(account_sequence, [&result](Result<HoldingsSnapshot> value) {
      result.emplace(std::move(value));
    });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category = ErrorCategory::TRANSPORT;
      error.broker_code = "event_loop_stopped";
      error.message = "event loop stopped before holdings completion";
      return Result<HoldingsSnapshot>(std::move(error));
    }
    return std::move(*result);
  }

private:
  net::io_context ioc_;
  std::shared_ptr<TossClient> client_;
};

} // namespace algotradelib::tossinvest
