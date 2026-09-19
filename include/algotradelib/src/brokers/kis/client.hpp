#pragma once

#include "./config.hpp"
#include "../common.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <cstdlib>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib::kis {

/** Selects one KIS trading account for a holdings request. */
struct KisAccount {
  std::string account_number_;
  std::string product_code_;

  /** Loads account selection from `KIS_CANO` and optional `KIS_ACNT_PRDT_CD`. */
  void from_env() {
    const char* account_number = std::getenv("KIS_CANO");
    if (!account_number) {
      throw std::runtime_error("KIS_CANO not set (8-digit account number)");
    }
    const char* product_code = std::getenv("KIS_ACNT_PRDT_CD");
    account_number_ = account_number;
    product_code_ = product_code ? product_code : "01";
  }
};

/** Persistent asynchronous KIS client with coalesced OAuth refresh. */
class KisClient : public std::enable_shared_from_this<KisClient> {
public:
  /** Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;

  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;

  /** Internal completion handler for a shared access-token refresh. */
  using TokenHandler = net::any_completion_handler<void(std::optional<BrokerError>)>;

  /**
   * Constructs a client for one KIS environment.
   *
   * `config` is a value sink because the client retains its own credentials;
   * lvalues are copied once and rvalues are moved into the client.
   */
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

  /** Requests and normalizes all holdings for the selected KIS account. */
  template <typename CompletionToken>
  auto async_holdings(KisAccount account, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<HoldingsSnapshot>)>(
      [self = shared_from_this(), account = std::move(account)](auto&& handler) mutable {
        auto completion = HoldingsHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, account = std::move(account), completion = std::move(completion)]() mutable {
            self->start_holdings(std::move(account), std::move(completion));
          }
        );
      },
      token
    );
  }

  /** Cancels transport work and queued HTTP operations. */
  void close() { http_->close(); }

private:
  friend struct KisClientTestAccess;

  net::strand<net::any_io_executor> strand_;
  KisConfig config_;
  std::shared_ptr<network::AsyncHttpClient> http_;
  KisRest token_;
  bool refreshing_{false};
  std::deque<TokenHandler> token_waiters_;

  struct HoldingsOperation {
    KisAccount account_;
    HoldingsSnapshot snapshot_;
    std::string foreign_cursor_;
    std::string domestic_cursor_;
    std::size_t page_count_{0};
    HoldingsHandler completion_;

    HoldingsOperation(KisAccount selected_account, HoldingsHandler selected_completion)
    : account_(std::move(selected_account)), completion_(std::move(selected_completion)) {}
  };

  void start_quote(InstrumentId instrument, QuoteHandler completion) {
    if (instrument.country_ != Country::KR || !broker_detail::valid_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS quote currently supports Korean equity symbols only";
      complete(std::move(completion), Result<QuoteSnapshot>(std::move(error)));
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

  void start_holdings(KisAccount account, HoldingsHandler completion) {
    if (
      !broker_detail::decimal_digits(account.account_number_, 8) ||
      !broker_detail::decimal_digits(account.product_code_, 2)
    ) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_account";
      error.message_ = "KIS account number must contain eight digits and product code two digits";
      complete_holdings(
        std::move(completion), Result<HoldingsSnapshot>(std::move(error))
      );
      return;
    }

    auto operation = std::make_shared<HoldingsOperation>(
      std::move(account), std::move(completion)
    );
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->finish_holdings(operation, Result<HoldingsSnapshot>(std::move(*token_error)));
            return;
          }
          self->request_holdings(operation);
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
    request.target_ = "/oauth2/tokenP";
    request.body_ = config_.to_jsonbody();
    request.headers_.emplace_back("content-type", "application/json");
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
    } else if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      failure = broker_detail::http_error(result.value());
      failure->category_ = ErrorCategory::AUTHENTICATION;
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
    const char* market = "UN";
    if (instrument.venue_ == Venue::KRX) {
      market = "J";
    } else if (instrument.venue_ == Venue::NXT) {
      market = "NX";
    }

    network::HttpRequest request;
    request.method_ = http::verb::get;
    request.target_ =
      "/uapi/domestic-stock/v1/quotations/inquire-price"
      "?FID_COND_MRKT_DIV_CODE=" +
      std::string(market) + "&FID_INPUT_ISCD=" + instrument.symbol_;
    request.headers_ = {
      {"authorization", "Bearer " + token_.access_token_},
      {"appkey", config_.key()},
      {"appsecret", config_.secret()},
      {"tr_id", "FHKST01010100"},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
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

  void request_holdings(const std::shared_ptr<HoldingsOperation>& operation) {
    auto request = make_holdings_request(
      operation->account_,
      config_,
      token_.access_token_,
      operation->foreign_cursor_,
      operation->domestic_cursor_,
      operation->page_count_ > 0
    );
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), operation](Result<network::HttpResponse> result) mutable {
        net::post(
          self->strand_,
          [self, operation, result = std::move(result)]() mutable {
            self->handle_holdings_page(operation, std::move(result));
          }
        );
      }
    );
  }

  static network::HttpRequest make_holdings_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    std::string_view foreign_cursor,
    std::string_view domestic_cursor,
    bool continuation
  ) {
    network::HttpRequest request;
    request.method_ = http::verb::get;
    request.target_ =
      "/uapi/domestic-stock/v1/trading/inquire-balance"
      "?CANO=" +
      account.account_number_ + "&ACNT_PRDT_CD=" + account.product_code_ +
      "&AFHR_FLPR_YN=Y&INQR_DVSN=01&UNPR_DVSN=01&FUND_STTL_ICLD_YN=N"
      "&FNCG_AMT_AUTO_RDPT_YN=N&PRCS_DVSN=00&CTX_AREA_FK100=" +
      broker_detail::query_encode(foreign_cursor) + "&CTX_AREA_NK100=" +
      broker_detail::query_encode(domestic_cursor);
    request.headers_ = {
      {"authorization", "Bearer " + std::string(access_token)},
      {"appkey", config.key()},
      {"appsecret", config.secret()},
      {"tr_id", config.is_demo_ ? "VTTC8434R" : "TTTC8434R"},
      {"tr_cont", continuation ? "N" : ""},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
    return request;
  }

  static std::optional<BrokerError> append_holdings_page(
    HoldingsSnapshot& snapshot,
    std::size_t& page_count,
    std::string& foreign_cursor,
    std::string& domestic_cursor,
    HoldingsSnapshot page,
    std::string next_foreign_cursor,
    std::string next_domestic_cursor,
    bool has_next
  ) {
    if (has_next) {
      const bool cursor_did_not_advance = next_foreign_cursor == foreign_cursor &&
        next_domestic_cursor == domestic_cursor;
      if (
        page_count + 1 >= 100 ||
        (next_foreign_cursor.empty() && next_domestic_cursor.empty()) ||
        cursor_did_not_advance
      ) {
        BrokerError error;
        error.category_ = ErrorCategory::PROTOCOL;
        error.broker_code_ = "invalid_pagination";
        error.message_ = "KIS holdings continuation did not advance";
        return error;
      }
    }

    snapshot.positions_.insert(
      snapshot.positions_.end(),
      std::make_move_iterator(page.positions_.begin()),
      std::make_move_iterator(page.positions_.end())
    );
    if (!page.totals_.empty()) {
      snapshot.totals_ = std::move(page.totals_);
    }
    ++page_count;
    if (has_next) {
      foreign_cursor = std::move(next_foreign_cursor);
      domestic_cursor = std::move(next_domestic_cursor);
    }
    return std::nullopt;
  }

  void handle_holdings_page(const std::shared_ptr<HoldingsOperation>& operation, Result<network::HttpResponse> result) {
    if (!result) {
      finish_holdings(operation, Result<HoldingsSnapshot>(std::move(result.error())));
      return;
    }

    const auto continuation = result.value().base().find("tr_cont");
    const bool has_next = continuation != result.value().base().end() &&
      (continuation->value() == "M" || continuation->value() == "F");
    std::string foreign_cursor;
    std::string domestic_cursor;
    auto page = parse_holdings_page(
      std::move(result), foreign_cursor, domestic_cursor
    );
    if (!page) {
      finish_holdings(operation, Result<HoldingsSnapshot>(std::move(page.error())));
      return;
    }

    auto pagination_error = append_holdings_page(
      operation->snapshot_,
      operation->page_count_,
      operation->foreign_cursor_,
      operation->domestic_cursor_,
      std::move(page.value()),
      std::move(foreign_cursor),
      std::move(domestic_cursor),
      has_next
    );
    if (pagination_error) {
      finish_holdings(
        operation, Result<HoldingsSnapshot>(std::move(*pagination_error))
      );
      return;
    }

    if (has_next) {
      request_holdings(operation);
      return;
    }

    operation->snapshot_.received_at_ = std::chrono::system_clock::now();
    finish_holdings(operation, Result<HoldingsSnapshot>(std::move(operation->snapshot_)));
  }

  static Result<QuoteSnapshot> parse_quote(InstrumentId instrument, Result<network::HttpResponse> result) {
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
        error.category_ = ErrorCategory::PROTOCOL;
        if (const auto code = document->if_contains("msg_cd"); code && code->is_string()) {
          error.broker_code_ = code->as_string().c_str();
        }
        if (const auto message = document->if_contains("msg1"); message && message->is_string()) {
          error.message_ = message->as_string().c_str();
        }
        return Result<QuoteSnapshot>(std::move(error));
      }
      const auto output = document->if_contains("output");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output");
      }
      if (
        const auto symbol = output->as_object().if_contains("stck_shrn_iscd");
        symbol && symbol->is_string() && symbol->as_string() != instrument.symbol_
      ) {
        throw std::runtime_error("quote response symbol does not match request");
      }
      QuoteSnapshot quote;
      quote.instrument_ = std::move(instrument);
      quote.last_price_ =
        Decimal64::parse(broker_detail::json_string(output->as_object(), "stck_prpr"));
      quote.currency_ = Currency::krw();
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

  static Result<HoldingsSnapshot> parse_holdings(Result<network::HttpResponse> result) {
    std::string foreign_cursor;
    std::string domestic_cursor;
    return parse_holdings_page(
      std::move(result), foreign_cursor, domestic_cursor
    );
  }

  static Result<HoldingsSnapshot> parse_holdings_page(
    Result<network::HttpResponse> result,
    std::string& foreign_cursor,
    std::string& domestic_cursor
  ) {
    if (!result) {
      return Result<HoldingsSnapshot>(std::move(result.error()));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<HoldingsSnapshot>(broker_detail::http_error(result.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<HoldingsSnapshot>(std::move(parse_error));
    }
    try {
      const auto result_code = broker_detail::json_string(*document, "rt_cd");
      if (result_code != "0") {
        BrokerError error;
        error.category_ = ErrorCategory::PROTOCOL;
        if (const auto code = document->if_contains("msg_cd"); code && code->is_string()) {
          error.broker_code_ = code->as_string().c_str();
        }
        if (const auto message = document->if_contains("msg1"); message && message->is_string()) {
          error.message_ = message->as_string().c_str();
        }
        return Result<HoldingsSnapshot>(std::move(error));
      }

      const auto positions = document->if_contains("output1");
      const auto summaries = document->if_contains("output2");
      if (!positions || !positions->is_array()) {
        throw std::runtime_error("missing array field: output1");
      }
      if (!summaries || !summaries->is_array()) {
        throw std::runtime_error("missing array field: output2");
      }

      HoldingsSnapshot snapshot;
      for (const auto& value : positions->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("output1 item is not an object");
        }
        const auto& output = value.as_object();
        HoldingPosition position;
        position.instrument_ = InstrumentId{
          Country::KR,
          Venue::COMBINED,
          broker_detail::json_string(output, "pdno"),
        };
        position.name_ = broker_detail::json_string(output, "prdt_name");
        position.quantity_ = Decimal64::parse(broker_detail::json_string(output, "hldg_qty"));
        position.sellable_quantity_ =
          Decimal64::parse(broker_detail::json_string(output, "ord_psbl_qty"));
        position.average_purchase_price_ =
          Decimal64::parse(broker_detail::json_string(output, "pchs_avg_pric"));
        position.last_price_ = Decimal64::parse(broker_detail::json_string(output, "prpr"));
        position.purchase_value_ = Money{
          Decimal64::parse(broker_detail::json_string(output, "pchs_amt")), Currency::krw()
        };
        position.market_value_ = Money{
          Decimal64::parse(broker_detail::json_string(output, "evlu_amt")), Currency::krw()
        };
        position.unrealized_profit_loss_ = Money{
          Decimal64::parse(broker_detail::json_string(output, "evlu_pfls_amt")), Currency::krw()
        };
        position.unrealized_return_ =
          broker_detail::percent_to_ratio(broker_detail::json_string(output, "evlu_pfls_rt"));
        snapshot.positions_.push_back(std::move(position));
      }

      if (!summaries->as_array().empty()) {
        const auto& value = summaries->as_array().front();
        if (!value.is_object()) {
          throw std::runtime_error("output2 item is not an object");
        }
        const auto& output = value.as_object();
        snapshot.totals_.push_back(
          HoldingsTotals{
            Currency::krw(),
            Decimal64::parse(broker_detail::json_string(output, "pchs_amt_smtl_amt")),
            Decimal64::parse(broker_detail::json_string(output, "evlu_amt_smtl_amt")),
            Decimal64::parse(broker_detail::json_string(output, "evlu_pfls_smtl_amt")),
          }
        );
      }

      if (
        const auto value = document->if_contains("ctx_area_fk100"); value && value->is_string()
      ) {
        foreign_cursor = value->as_string().c_str();
      }
      if (
        const auto value = document->if_contains("ctx_area_nk100"); value && value->is_string()
      ) {
        domestic_cursor = value->as_string().c_str();
      }
      return Result<HoldingsSnapshot>(std::move(snapshot));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_holdings_response";
      error.message_ = exception.what();
      return Result<HoldingsSnapshot>(std::move(error));
    }
  }

  void complete(QuoteHandler handler, Result<QuoteSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void finish_holdings(
    const std::shared_ptr<HoldingsOperation>& operation,
    Result<HoldingsSnapshot> result
  ) {
    complete_holdings(std::move(operation->completion_), std::move(result));
  }

  void complete_holdings(HoldingsHandler handler, Result<HoldingsSnapshot> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }
};

/** Blocking facade that drives `KisClient` on a private event loop. */
class KisBlockingClient {
public:
  /** Constructs a blocking client for one KIS environment. */
  KisBlockingClient(
    ssl::context& context,
    KisConfig config,
    network::HttpClientOptions options = {}
  )
  : client_(std::make_shared<KisClient>(ioc_.get_executor(), context, std::move(config), options)) {}

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
      error.category_ = ErrorCategory::TRANSPORT;
      error.broker_code_ = "event_loop_stopped";
      error.message_ = "event loop stopped before quote completion";
      return Result<QuoteSnapshot>(std::move(error));
    }
    return std::move(*result);
  }

  /** Returns all normalized holdings for the selected KIS account. */
  Result<HoldingsSnapshot> holdings(KisAccount account) {
    std::optional<Result<HoldingsSnapshot>> result;
    ioc_.restart();
    client_->async_holdings(std::move(account), [&result](Result<HoldingsSnapshot> value) {
      result.emplace(std::move(value));
    });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      error.broker_code_ = "event_loop_stopped";
      error.message_ = "event loop stopped before holdings completion";
      return Result<HoldingsSnapshot>(std::move(error));
    }
    return std::move(*result);
  }

private:
  net::io_context ioc_;
  std::shared_ptr<KisClient> client_;
};

} // namespace algotradelib::kis
