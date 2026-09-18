#pragma once

#include "./config.hpp"
#include "../common.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace algotradelib::tossinvest {

class TossClient : public std::enable_shared_from_this<TossClient> {
public:
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;
  using TokenHandler = net::any_completion_handler<void(Result<std::string>)>;

  TossClient(net::any_io_executor executor, ssl::context& context, TossConfig config,
             network::HttpClientOptions options = {})
      : strand_(net::make_strand(std::move(executor))), config_(std::move(config)),
        http_(std::make_shared<network::AsyncHttpClient>(
          strand_, context, "openapi.tossinvest.com", "443", options)) {}

  template <typename CompletionToken>
  auto async_quote(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<QuoteSnapshot>)>(
      [self = shared_from_this(), instrument = std::move(instrument)](auto&& handler) mutable {
        auto completion = QuoteHandler(std::forward<decltype(handler)>(handler));
        net::post(self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion)]() mutable {
            self->start_quote(std::move(instrument), std::move(completion));
          });
      }, token);
  }

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
    if (instrument.country != Country::kr ||
        !broker_detail::valid_symbol(instrument.symbol)) {
      BrokerError error;
      error.category = ErrorCategory::invalid_argument;
      error.broker_code = "invalid_instrument";
      error.message = "Toss quote currently supports Korean equity symbols only";
      complete(std::move(completion), Result<QuoteSnapshot>(std::move(error)));
      return;
    }
    ensure_token(TokenHandler(
      [self = shared_from_this(), instrument = std::move(instrument),
       completion = std::move(completion)](Result<std::string> token_result) mutable {
        if (!token_result) {
          self->complete(std::move(completion),
                         Result<QuoteSnapshot>(std::move(token_result.error())));
          return;
        }
        self->request_quote(std::move(instrument), std::move(token_result.value()),
                            std::move(completion));
      }));
  }

  void ensure_token(TokenHandler handler) {
    if (token_.valid()) {
      std::move(handler)(Result<std::string>(token_.access_token_));
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
    http_->async_request(std::move(request),
      [self = shared_from_this()](Result<network::HttpResponse> result) mutable {
        net::post(self->strand_, [self, result = std::move(result)]() mutable {
          self->finish_token(std::move(result));
        });
      });
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
        failure = broker_detail::toss_error(*document, result.value().result_int());
        failure->category = ErrorCategory::authentication;
      } else {
        try {
          token_.from_json(*document);
        } catch (const std::exception& exception) {
          BrokerError error;
          error.category = ErrorCategory::authentication;
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
        std::move(waiter)(Result<std::string>(*failure));
      } else {
        std::move(waiter)(Result<std::string>(token_.access_token_));
      }
    }
  }

  void request_quote(InstrumentId instrument, std::string access_token,
                     QuoteHandler completion) {
    network::HttpRequest request;
    request.method = http::verb::get;
    request.target = "/api/v1/prices?symbols=" + instrument.symbol;
    request.headers.emplace_back("authorization", "Bearer " + access_token);
    http_->async_request(std::move(request),
      [self = shared_from_this(), instrument = std::move(instrument),
       completion = std::move(completion)](Result<network::HttpResponse> result) mutable {
        net::post(self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion),
           result = std::move(result)]() mutable {
            self->complete(std::move(completion),
              parse_quote(std::move(instrument), std::move(result)));
          });
      });
  }

  static Result<QuoteSnapshot> parse_quote(
      InstrumentId instrument, Result<network::HttpResponse> result) {
    if (!result) {
      return Result<QuoteSnapshot>(std::move(result.error()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<QuoteSnapshot>(std::move(parse_error));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      auto error = broker_detail::toss_error(*document, result.value().result_int());
      error.retry_after = broker_detail::retry_after(result.value());
      return Result<QuoteSnapshot>(std::move(error));
    }
    try {
      const auto payload = document->if_contains("result");
      if (!payload || !payload->is_array() || payload->as_array().empty() ||
          !payload->as_array().front().is_object()) {
        throw std::runtime_error("missing non-empty result array");
      }
      const auto& output = payload->as_array().front().as_object();
      if (broker_detail::json_string(output, "symbol") != instrument.symbol) {
        throw std::runtime_error("quote response symbol does not match request");
      }
      QuoteSnapshot quote;
      quote.instrument = std::move(instrument);
      quote.last_price = Decimal64::parse(
        broker_detail::json_string(output, "lastPrice"));
      quote.currency = Currency(broker_detail::json_string(output, "currency"));
      if (const auto timestamp = output.if_contains("timestamp");
          timestamp && timestamp->is_string()) {
        quote.exchange_time = parse_iso8601_timestamp(timestamp->as_string().c_str());
        if (!quote.exchange_time) {
          throw std::runtime_error("invalid timestamp");
        }
      }
      quote.received_at = std::chrono::system_clock::now();
      return Result<QuoteSnapshot>(std::move(quote));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category = ErrorCategory::protocol;
      error.broker_code = "invalid_quote_response";
      error.message = exception.what();
      return Result<QuoteSnapshot>(std::move(error));
    }
  }

  void complete(QuoteHandler handler, Result<QuoteSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor,
      [handler = std::move(handler), result = std::move(result)]() mutable {
        std::move(handler)(std::move(result));
      });
  }
};

class TossBlockingClient {
public:
  TossBlockingClient(ssl::context& context, TossConfig config,
                     network::HttpClientOptions options = {})
      : client_(std::make_shared<TossClient>(ioc_.get_executor(), context,
                                             std::move(config), options)) {}

  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    std::optional<Result<QuoteSnapshot>> result;
    ioc_.restart();
    client_->async_quote(std::move(instrument),
      [&result](Result<QuoteSnapshot> value) { result.emplace(std::move(value)); });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category = ErrorCategory::transport;
      error.broker_code = "event_loop_stopped";
      error.message = "event loop stopped before quote completion";
      return Result<QuoteSnapshot>(std::move(error));
    }
    return std::move(*result);
  }

private:
  net::io_context ioc_;
  std::shared_ptr<TossClient> client_;
};

} // namespace algotradelib::tossinvest
