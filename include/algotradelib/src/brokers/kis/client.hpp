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

namespace algotradelib::kis {

class KisClient : public std::enable_shared_from_this<KisClient> {
public:
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;
  using TokenHandler = net::any_completion_handler<void(Result<std::string>)>;

  KisClient(
    net::any_io_executor executor,
    ssl::context& context,
    KisConfig config,
    network::HttpClientOptions options = {}
  )
  : strand_(net::make_strand(std::move(executor))), config_(std::move(config)),
    http_(
      std::make_shared<network::AsyncHttpClient>(
        strand_,
        context,
        config_.rest_host(),
        config_.rest_port(),
        options
      )
    ) {}

  template <typename CompletionToken>
  auto async_quote(
    InstrumentId instrument,
    CompletionToken&& token
  ) {
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

  void close() { http_->close(); }

private:
  friend struct KisClientTestAccess;

  net::strand<net::any_io_executor> strand_;
  KisConfig config_;
  std::shared_ptr<network::AsyncHttpClient> http_;
  KisRest token_;
  bool refreshing_{false};
  std::deque<TokenHandler> token_waiters_;

  void start_quote(
    InstrumentId instrument,
    QuoteHandler completion
  ) {
    if (instrument.country != Country::KR || !broker_detail::valid_symbol(instrument.symbol)) {
      BrokerError error;
      error.category = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code = "invalid_instrument";
      error.message = "KIS quote currently supports Korean equity symbols only";
      complete(std::move(completion), Result<QuoteSnapshot>(std::move(error)));
      return;
    }

    ensure_token(
      TokenHandler(
        [self = shared_from_this(),
          instrument = std::move(instrument),
          completion = std::move(completion)](Result<std::string> token_result) mutable {
          if (!token_result) {
            self->complete(
              std::move(completion), Result<QuoteSnapshot>(std::move(token_result.error()))
            );
            return;
          }
          self->request_quote(
            std::move(instrument), std::move(token_result.value()), std::move(completion)
          );
        }
      )
    );
  }

  void ensure_token(
    TokenHandler handler
  ) {
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
    request.target = "/oauth2/tokenP";
    request.body = config_.to_jsonbody();
    request.headers.emplace_back("content-type", "application/json");
    http_->async_request(
      std::move(request),
      [self = shared_from_this()](Result<network::HttpResponse> result) mutable {
        net::post(self->strand_, [self, result = std::move(result)]() mutable {
          self->finish_token(std::move(result));
        });
      }
    );
  }

  void finish_token(
    Result<network::HttpResponse> result
  ) {
    refreshing_ = false;
    std::optional<BrokerError> failure;
    if (!result) {
      failure = std::move(result.error());
    } else if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      failure = broker_detail::http_error(result.value());
      failure->category = ErrorCategory::AUTHENTICATION;
    } else {
      BrokerError parse_error;
      auto document = broker_detail::parse_object(result.value().body(), parse_error);
      if (!document) {
        failure = std::move(parse_error);
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
        std::move(waiter)(Result<std::string>(*failure));
      } else {
        std::move(waiter)(Result<std::string>(token_.access_token_));
      }
    }
  }

  void request_quote(
    InstrumentId instrument,
    std::string access_token,
    QuoteHandler completion
  ) {
    const char* market = "UN";
    if (instrument.venue == Venue::KRX) {
      market = "J";
    } else if (instrument.venue == Venue::NXT) {
      market = "NX";
    }

    network::HttpRequest request;
    request.method = http::verb::get;
    request.target =
      "/uapi/domestic-stock/v1/quotations/inquire-price"
      "?FID_COND_MRKT_DIV_CODE=" +
      std::string(market) + "&FID_INPUT_ISCD=" + instrument.symbol;
    request.headers = {
      {"authorization", "Bearer " + access_token},
      {"appkey", config_.key()},
      {"appsecret", config_.secret()},
      {"tr_id", "FHKST01010100"},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
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

  static Result<QuoteSnapshot> parse_quote(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      return Result<QuoteSnapshot>(std::move(result.error()));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<QuoteSnapshot>(broker_detail::http_error(result.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<QuoteSnapshot>(std::move(parse_error));
    }
    try {
      const auto result_code = broker_detail::json_string(*document, "rt_cd");
      if (result_code != "0") {
        BrokerError error;
        error.category = ErrorCategory::PROTOCOL;
        if (const auto code = document->if_contains("msg_cd"); code && code->is_string()) {
          error.broker_code = code->as_string().c_str();
        }
        if (const auto message = document->if_contains("msg1"); message && message->is_string()) {
          error.message = message->as_string().c_str();
        }
        return Result<QuoteSnapshot>(std::move(error));
      }
      const auto output = document->if_contains("output");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output");
      }
      if (
        const auto symbol = output->as_object().if_contains("stck_shrn_iscd");
        symbol && symbol->is_string() && symbol->as_string() != instrument.symbol
      ) {
        throw std::runtime_error("quote response symbol does not match request");
      }
      QuoteSnapshot quote;
      quote.instrument = std::move(instrument);
      quote.last_price =
        Decimal64::parse(broker_detail::json_string(output->as_object(), "stck_prpr"));
      quote.currency = Currency::krw();
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

  void complete(
    QuoteHandler handler,
    Result<QuoteSnapshot> result
  ) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }
};

class KisBlockingClient {
public:
  KisBlockingClient(
    ssl::context& context,
    KisConfig config,
    network::HttpClientOptions options = {}
  )
  : client_(std::make_shared<KisClient>(ioc_.get_executor(), context, std::move(config), options)) {
  }

  Result<QuoteSnapshot> quote(
    InstrumentId instrument
  ) {
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

private:
  net::io_context ioc_;
  std::shared_ptr<KisClient> client_;
};

} // namespace algotradelib::kis
