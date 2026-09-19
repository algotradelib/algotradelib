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

/**
 * Identifies one accepted KIS order.
 *
 * KIS addresses an order by branch code together with order number, so an
 * order reference is not reducible to a single opaque string. Flattening the
 * two into a synthetic composite identifier would be an invention rather than
 * a normalization, so both parts are preserved.
 */
struct KisOrderRef {
  std::string branch_number_;
  std::string order_number_;
};

/**
 * Quantity and price instruction for a KIS modification or cancellation.
 *
 * `all_remaining_` selects KIS's `QTY_ALL_ORD_YN` flag. The caller states
 * explicitly whether the whole remaining quantity or only `quantity_` is
 * affected; the adapter never infers it, because guessing would silently
 * change how much of a live order is withdrawn.
 *
 * KIS documents that the amendable quantity must first be read from
 * `async_open_orders`, which reports it as `amendable_quantity_`.
 */
struct KisAmendment {
  KisOrderRef order_;
  OrderType type_{OrderType::LIMIT};
  Decimal64 quantity_;
  std::optional<Decimal64> price_;
  bool all_remaining_{false};
};

/**
 * KIS acknowledgement of an accepted order, modification, or cancellation.
 *
 * Acceptance means KIS recorded the instruction, not that it executed. The
 * resulting state must be read back through an order inquiry.
 *
 * `broker_order_time_` is KIS's `ORD_TMD`, an `HHMMSS` string with no date or
 * zone. It is preserved verbatim rather than being combined with a guessed
 * calendar date; `ack_.accepted_at_` records local receipt instead.
 */
struct KisOrderAck {
  OrderAck ack_;
  KisOrderRef order_;
  std::string broker_order_time_;
};

/** One KIS order that is still open to modification or cancellation. */
struct KisOpenOrder {
  OrderSnapshot order_;
  KisOrderRef order_ref_;
  KisOrderRef original_order_;
  Decimal64 amendable_quantity_;
};

/** One page of KIS orders that remain modifiable or cancellable. */
struct KisOpenOrderPage {
  std::vector<KisOpenOrder> orders_;
  std::chrono::system_clock::time_point received_at_;
};

/** Inclusive calendar range for a KIS order-history inquiry. */
struct KisHistoryRange {
  Date from_;
  Date to_;
};

/** Persistent asynchronous KIS client with coalesced OAuth refresh. */
class KisClient : public std::enable_shared_from_this<KisClient> {
public:
  /** Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;

  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;

  /** Completion handler for one KIS order acknowledgement. */
  using OrderAckHandler = net::any_completion_handler<void(Result<KisOrderAck>)>;

  /** Completion handler for one page of modifiable or cancellable orders. */
  using OpenOrderHandler = net::any_completion_handler<void(Result<KisOpenOrderPage>)>;

  /** Completion handler for one normalized page of order history. */
  using OrderPageHandler = net::any_completion_handler<void(Result<OrderPage>)>;

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

  /**
   * Submits a new cash order and returns KIS's acknowledgement.
   *
   * The order POST is never replayed automatically. A `TRANSPORT` error
   * therefore leaves the outcome *indeterminate*: the order may or may not
   * have reached KIS. The caller must reconcile against `async_open_orders`
   * or `async_order_history` before resubmitting, because KIS offers no
   * idempotency key for individual customers.
   */
  template <typename CompletionToken>
  auto async_place_order(KisAccount account, OrderRequest order, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KisOrderAck>)>(
      [
        self = shared_from_this(),
        account = std::move(account),
        order = std::move(order)
      ](auto&& handler) mutable {
        auto completion = OrderAckHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account = std::move(account),
            order = std::move(order),
            completion = std::move(completion)
          ]() mutable {
            self->start_place_order(std::move(account), std::move(order), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Modifies the price or quantity of an existing order.
   *
   * The same indeterminacy rule as `async_place_order` applies: a transport
   * failure does not mean the modification was rejected.
   */
  template <typename CompletionToken>
  auto async_replace_order(KisAccount account, KisAmendment amendment, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KisOrderAck>)>(
      [
        self = shared_from_this(),
        account = std::move(account),
        amendment = std::move(amendment)
      ](auto&& handler) mutable {
        auto completion = OrderAckHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account = std::move(account),
            amendment = std::move(amendment),
            completion = std::move(completion)
          ]() mutable {
            self->start_amend_order(
              std::move(account), std::move(amendment), false, std::move(completion)
            );
          }
        );
      },
      token
    );
  }

  /**
   * Cancels an existing order.
   *
   * The same indeterminacy rule as `async_place_order` applies: a transport
   * failure does not mean the order is still live.
   */
  template <typename CompletionToken>
  auto async_cancel_order(KisAccount account, KisAmendment amendment, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KisOrderAck>)>(
      [
        self = shared_from_this(),
        account = std::move(account),
        amendment = std::move(amendment)
      ](auto&& handler) mutable {
        auto completion = OrderAckHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account = std::move(account),
            amendment = std::move(amendment),
            completion = std::move(completion)
          ]() mutable {
            self->start_amend_order(
              std::move(account), std::move(amendment), true, std::move(completion)
            );
          }
        );
      },
      token
    );
  }

  /**
   * Retrieves every order that is still modifiable or cancellable.
   *
   * KIS does not offer this inquiry on demo accounts, so a demo configuration
   * fails with `ErrorCategory::INVALID_ARGUMENT` instead of issuing a request
   * that cannot succeed.
   */
  template <typename CompletionToken>
  auto async_open_orders(KisAccount account, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KisOpenOrderPage>)>(
      [self = shared_from_this(), account = std::move(account)](auto&& handler) mutable {
        auto completion = OpenOrderHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, account = std::move(account), completion = std::move(completion)]() mutable {
            self->start_open_orders(std::move(account), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Retrieves placed orders and their executions over an inclusive date range.
   *
   * KIS reports no order status on this endpoint; the status of each returned
   * order is derived from its quantity, fill, remainder, rejection, and
   * cancellation columns.
   */
  template <typename CompletionToken>
  auto async_order_history(KisAccount account, KisHistoryRange range, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<OrderPage>)>(
      [
        self = shared_from_this(),
        account = std::move(account),
        range
      ](auto&& handler) mutable {
        auto completion = OrderPageHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [
            self,
            account = std::move(account),
            range,
            completion = std::move(completion)
          ]() mutable {
            self->start_order_history(std::move(account), range, std::move(completion));
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
    if (auto invalid = validate_account(account)) {
      complete_holdings(
        std::move(completion), Result<HoldingsSnapshot>(std::move(*invalid))
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
      "&AFHR_FLPR_YN=Y&OFL_YN=&INQR_DVSN=01&UNPR_DVSN=01&FUND_STTL_ICLD_YN=N"
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

  struct OpenOrderOperation {
    KisAccount account_;
    KisOpenOrderPage page_;
    std::string foreign_cursor_;
    std::string domestic_cursor_;
    std::size_t page_count_{0};
    OpenOrderHandler completion_;

    OpenOrderOperation(KisAccount selected_account, OpenOrderHandler selected_completion)
    : account_(std::move(selected_account)), completion_(std::move(selected_completion)) {}
  };

  struct OrderHistoryOperation {
    KisAccount account_;
    KisHistoryRange range_;
    OrderPage page_;
    std::string foreign_cursor_;
    std::string domestic_cursor_;
    std::size_t page_count_{0};
    OrderPageHandler completion_;

    OrderHistoryOperation(
      KisAccount selected_account,
      KisHistoryRange selected_range,
      OrderPageHandler selected_completion
    )
    : account_(std::move(selected_account)), range_(selected_range),
      completion_(std::move(selected_completion)) {}
  };

  /** Returns an error when the account selector cannot address a KIS account. */
  static std::optional<BrokerError> validate_account(const KisAccount& account) {
    if (
      broker_detail::decimal_digits(account.account_number_, 8) &&
      broker_detail::decimal_digits(account.product_code_, 2)
    ) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "invalid_account";
    error.message_ = "KIS account number must contain eight digits and product code two digits";
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
    error.message_ = "KIS order quantity must be a positive whole number of shares";
    return error;
  }

  /**
   * Returns an error when a price does not match its order type.
   *
   * A limit order without a price and a market order carrying a price are both
   * rejected before any network I/O, because either would make the transmitted
   * order differ from the one the caller described.
   */
  static std::optional<BrokerError> validate_price(
    OrderType type,
    const std::optional<Decimal64>& price
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "invalid_price";
    if (type == OrderType::LIMIT) {
      if (!price) {
        error.message_ = "KIS limit order requires a price";
        return error;
      }
      if (price->coefficient() <= 0) {
        error.message_ = "KIS limit order price must be positive";
        return error;
      }
      if (price->scale() != 0) {
        error.message_ = "KIS domestic limit order price must be a whole number of won";
        return error;
      }
      return std::nullopt;
    }
    if (price) {
      error.message_ = "KIS market order must not carry a price";
      return error;
    }
    return std::nullopt;
  }

  /**
   * Maps a normalized order type to KIS's `ORD_DVSN` code.
   *
   * Only day-valid limit and market orders are mapped. An at-the-close or
   * at-the-open condition is rejected rather than downgraded to `DAY`, because
   * downgrading a validity condition changes when an order executes.
   */
  static std::optional<BrokerError> order_division(
    OrderType type,
    TimeInForce time_in_force,
    std::string& division
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "unsupported_order";
    if (time_in_force != TimeInForce::DAY) {
      error.message_ = "KIS cash orders currently support the DAY validity condition only";
      return error;
    }
    if (type == OrderType::LIMIT) {
      division = "00";
      return std::nullopt;
    }
    if (type == OrderType::MARKET) {
      division = "01";
      return std::nullopt;
    }
    error.message_ = "KIS cash orders currently support limit and market orders only";
    return error;
  }

  /** Returns whether a symbol has KIS's six-character domestic-stock form. */
  static bool valid_domestic_symbol(std::string_view symbol) {
    if (symbol.size() != 6) {
      return false;
    }
    for (const char ch : symbol) {
      const bool valid = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z');
      if (!valid) {
        return false;
      }
    }
    return true;
  }

  /** Returns an error when the order cannot be represented as a KIS cash order. */
  static std::optional<BrokerError> validate_order(const OrderRequest& order, std::string& division) {
    if (order.instrument_.country_ != Country::KR) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS cash orders currently support Korean equities only";
      return error;
    }
    if (!valid_domestic_symbol(order.instrument_.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS domestic order symbol must contain six letters or digits";
      return error;
    }
    if (auto error = validate_quantity(order.quantity_)) {
      return error;
    }
    if (auto error = validate_price(order.type_, order.price_)) {
      return error;
    }
    return order_division(order.type_, order.time_in_force_, division);
  }

  /** Returns KIS's venue code for the requested normalized venue. */
  static const char* exchange_code(Venue venue) {
    switch (venue) {
      case Venue::KRX:
        return "KRX";
      case Venue::NXT:
        return "NXT";
      default:
        return "";
    }
  }

  static network::HttpRequest make_place_order_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    const OrderRequest& order,
    std::string_view division
  ) {
    json::object body;
    body["CANO"] = account.account_number_;
    body["ACNT_PRDT_CD"] = account.product_code_;
    body["PDNO"] = order.instrument_.symbol_;
    body["ORD_DVSN"] = std::string(division);
    body["ORD_QTY"] = order.quantity_.str();
    body["ORD_UNPR"] = order.price_ ? order.price_->str() : std::string("0");
    if (const auto* exchange = exchange_code(order.instrument_.venue_); *exchange != '\0') {
      body["EXCG_ID_DVSN_CD"] = exchange;
    }

    const bool buy = order.side_ == OrderSide::BUY;
    network::HttpRequest request;
    request.method_ = http::verb::post;
    request.target_ = "/uapi/domestic-stock/v1/trading/order-cash";
    request.body_ = json::serialize(body);
    request.headers_ = {
      {"authorization", "Bearer " + std::string(access_token)},
      {"appkey", config.key()},
      {"appsecret", config.secret()},
      {"tr_id", buy ?
        (config.is_demo_ ? "VTTC0012U" : "TTTC0012U") :
        (config.is_demo_ ? "VTTC0011U" : "TTTC0011U")},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
    return request;
  }

  static network::HttpRequest make_amend_order_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    const KisAmendment& amendment,
    bool cancel,
    std::string_view division
  ) {
    json::object body;
    body["CANO"] = account.account_number_;
    body["ACNT_PRDT_CD"] = account.product_code_;
    body["KRX_FWDG_ORD_ORGNO"] = amendment.order_.branch_number_;
    body["ORGN_ODNO"] = amendment.order_.order_number_;
    body["ORD_DVSN"] = std::string(division);
    body["RVSE_CNCL_DVSN_CD"] = cancel ? "02" : "01";
    body["ORD_QTY"] = amendment.quantity_.str();
    body["ORD_UNPR"] = amendment.price_ ? amendment.price_->str() : std::string("0");
    body["QTY_ALL_ORD_YN"] = amendment.all_remaining_ ? "Y" : "N";

    network::HttpRequest request;
    request.method_ = http::verb::post;
    request.target_ = "/uapi/domestic-stock/v1/trading/order-rvsecncl";
    request.body_ = json::serialize(body);
    request.headers_ = {
      {"authorization", "Bearer " + std::string(access_token)},
      {"appkey", config.key()},
      {"appsecret", config.secret()},
      {"tr_id", config.is_demo_ ? "VTTC0013U" : "TTTC0013U"},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
    return request;
  }

  /** Returns KIS's declared failure for a response whose `rt_cd` is not `0`. */
  static std::optional<BrokerError> kis_declared_error(const json::object& document) {
    if (broker_detail::json_string(document, "rt_cd") == "0") {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    if (const auto code = document.if_contains("msg_cd"); code && code->is_string()) {
      error.broker_code_ = code->as_string().c_str();
    }
    if (const auto message = document.if_contains("msg1"); message && message->is_string()) {
      error.message_ = message->as_string().c_str();
    }
    return error;
  }

  static Result<KisOrderAck> parse_order_ack(Result<network::HttpResponse> result) {
    if (!result) {
      return Result<KisOrderAck>(std::move(result.error()));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<KisOrderAck>(broker_detail::http_error(result.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<KisOrderAck>(std::move(parse_error));
    }
    try {
      if (auto declared = kis_declared_error(*document)) {
        return Result<KisOrderAck>(std::move(*declared));
      }
      const auto output = document->if_contains("output");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output");
      }
      const auto& fields = output->as_object();

      KisOrderAck ack;
      ack.order_.branch_number_ = broker_detail::json_string(fields, "KRX_FWDG_ORD_ORGNO");
      ack.order_.order_number_ = broker_detail::json_string(fields, "ODNO");
      ack.broker_order_time_ = broker_detail::json_string(fields, "ORD_TMD");
      ack.ack_.order_id_ = ack.order_.order_number_;
      ack.ack_.accepted_at_ = std::chrono::system_clock::now();
      return Result<KisOrderAck>(std::move(ack));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_order_response";
      error.message_ = exception.what();
      return Result<KisOrderAck>(std::move(error));
    }
  }

  void start_place_order(KisAccount account, OrderRequest order, OrderAckHandler completion) {
    std::string division;
    auto invalid = validate_account(account);
    if (!invalid) {
      invalid = validate_order(order, division);
    }
    if (invalid) {
      complete_order_ack(std::move(completion), Result<KisOrderAck>(std::move(*invalid)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          account = std::move(account),
          order = std::move(order),
          division = std::move(division),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order_ack(
              std::move(completion), Result<KisOrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = make_place_order_request(
            account, self->config_, self->token_.access_token_, order, division
          );
          self->request_order_ack(std::move(request), std::move(completion));
        }
      )
    );
  }

  void start_amend_order(
    KisAccount account,
    KisAmendment amendment,
    bool cancel,
    OrderAckHandler completion
  ) {
    std::string division;
    auto invalid = validate_account(account);
    if (
      !invalid &&
      (!broker_detail::decimal_digits(amendment.order_.branch_number_, 5) ||
       !broker_detail::decimal_digits(amendment.order_.order_number_, 10))
    ) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_order_reference";
      error.message_ =
        "KIS amendment requires a five-digit branch number and ten-digit order number";
      invalid = std::move(error);
    }
    if (!invalid) {
      invalid = validate_quantity(amendment.quantity_);
    }
    if (!invalid) {
      invalid = validate_price(amendment.type_, amendment.price_);
    }
    if (!invalid) {
      invalid = order_division(amendment.type_, TimeInForce::DAY, division);
    }
    if (invalid) {
      complete_order_ack(std::move(completion), Result<KisOrderAck>(std::move(*invalid)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          account = std::move(account),
          amendment = std::move(amendment),
          cancel,
          division = std::move(division),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order_ack(
              std::move(completion), Result<KisOrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = make_amend_order_request(
            account, self->config_, self->token_.access_token_, amendment, cancel, division
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

  void start_open_orders(KisAccount account, OpenOrderHandler completion) {
    auto invalid = validate_account(account);
    if (!invalid && config_.is_demo_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "demo_unsupported";
      error.message_ = "KIS does not offer the modifiable-order inquiry on demo accounts";
      invalid = std::move(error);
    }
    if (invalid) {
      complete_open_orders(std::move(completion), Result<KisOpenOrderPage>(std::move(*invalid)));
      return;
    }

    auto operation = std::make_shared<OpenOrderOperation>(
      std::move(account), std::move(completion)
    );
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_open_orders(
              std::move(operation->completion_),
              Result<KisOpenOrderPage>(std::move(*token_error))
            );
            return;
          }
          self->request_open_orders(operation);
        }
      )
    );
  }

  static network::HttpRequest make_open_orders_request(
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
      "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl"
      "?CANO=" +
      account.account_number_ + "&ACNT_PRDT_CD=" + account.product_code_ +
      "&INQR_DVSN_1=0&INQR_DVSN_2=0&CTX_AREA_FK100=" +
      broker_detail::query_encode(foreign_cursor) + "&CTX_AREA_NK100=" +
      broker_detail::query_encode(domestic_cursor);
    request.headers_ = {
      {"authorization", "Bearer " + std::string(access_token)},
      {"appkey", config.key()},
      {"appsecret", config.secret()},
      {"tr_id", "TTTC0084R"},
      {"tr_cont", continuation ? "N" : ""},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
    return request;
  }

  void request_open_orders(const std::shared_ptr<OpenOrderOperation>& operation) {
    auto request = make_open_orders_request(
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
            self->handle_open_orders_page(operation, std::move(result));
          }
        );
      }
    );
  }

  void handle_open_orders_page(
    const std::shared_ptr<OpenOrderOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      complete_open_orders(
        std::move(operation->completion_), Result<KisOpenOrderPage>(std::move(result.error()))
      );
      return;
    }

    const bool has_next = continuation_pending(result.value());
    std::string foreign_cursor;
    std::string domestic_cursor;
    auto page = parse_open_orders_page(std::move(result), foreign_cursor, domestic_cursor);
    if (!page) {
      complete_open_orders(
        std::move(operation->completion_), Result<KisOpenOrderPage>(std::move(page.error()))
      );
      return;
    }

    auto pagination_error = advance_cursors(
      operation->page_count_,
      operation->foreign_cursor_,
      operation->domestic_cursor_,
      std::move(foreign_cursor),
      std::move(domestic_cursor),
      has_next,
      "KIS modifiable-order continuation did not advance"
    );
    if (pagination_error) {
      complete_open_orders(
        std::move(operation->completion_), Result<KisOpenOrderPage>(std::move(*pagination_error))
      );
      return;
    }

    operation->page_.orders_.insert(
      operation->page_.orders_.end(),
      std::make_move_iterator(page.value().orders_.begin()),
      std::make_move_iterator(page.value().orders_.end())
    );
    if (has_next) {
      request_open_orders(operation);
      return;
    }

    operation->page_.received_at_ = std::chrono::system_clock::now();
    complete_open_orders(
      std::move(operation->completion_), Result<KisOpenOrderPage>(std::move(operation->page_))
    );
  }

  static Result<KisOpenOrderPage> parse_open_orders_page(
    Result<network::HttpResponse> result,
    std::string& foreign_cursor,
    std::string& domestic_cursor
  ) {
    if (!result) {
      return Result<KisOpenOrderPage>(std::move(result.error()));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<KisOpenOrderPage>(broker_detail::http_error(result.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<KisOpenOrderPage>(std::move(parse_error));
    }
    try {
      if (auto declared = kis_declared_error(*document)) {
        return Result<KisOpenOrderPage>(std::move(*declared));
      }
      const auto rows = document->if_contains("output");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: output");
      }

      KisOpenOrderPage page;
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("output item is not an object");
        }
        const auto& row = value.as_object();

        KisOpenOrder open_order;
        open_order.order_ref_.branch_number_ = broker_detail::json_string(row, "ord_gno_brno");
        open_order.order_ref_.order_number_ = broker_detail::json_string(row, "odno");
        open_order.original_order_.branch_number_ = open_order.order_ref_.branch_number_;
        open_order.original_order_.order_number_ = broker_detail::json_string(row, "orgn_odno");
        open_order.amendable_quantity_ =
          Decimal64::parse(broker_detail::json_string(row, "psbl_qty"));

        auto& order = open_order.order_;
        order.order_id_ = open_order.order_ref_.order_number_;
        order.instrument_ = InstrumentId{
          Country::KR, Venue::COMBINED, broker_detail::json_string(row, "pdno")
        };
        order.name_ = broker_detail::json_string(row, "prdt_name");
        order.side_ = normalized_order_side(
          broker_detail::json_string(row, "sll_buy_dvsn_cd")
        );
        order.type_ = normalized_order_type(broker_detail::json_string(row, "ord_dvsn_cd"));
        order.time_in_force_ = order.type_ == OrderType::UNKNOWN ?
          TimeInForce::UNKNOWN :
          TimeInForce::DAY;
        order.quantity_ = Decimal64::parse(broker_detail::json_string(row, "ord_qty"));
        if (order.type_ == OrderType::LIMIT) {
          order.price_ = Decimal64::parse(broker_detail::json_string(row, "ord_unpr"));
        }
        order.currency_ = Currency::krw();
        order.execution_.filled_quantity_ =
          Decimal64::parse(broker_detail::json_string(row, "tot_ccld_qty"));
        order.execution_.filled_amount_ = Money{
          Decimal64::parse(broker_detail::json_string(row, "tot_ccld_amt")), Currency::krw()
        };
        order.status_ = order.execution_.filled_quantity_.coefficient() > 0 ?
          OrderStatus::PARTIAL_FILLED :
          OrderStatus::PENDING;
        page.orders_.push_back(std::move(open_order));
      }

      read_cursors(*document, foreign_cursor, domestic_cursor);
      return Result<KisOpenOrderPage>(std::move(page));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_open_order_response";
      error.message_ = exception.what();
      return Result<KisOpenOrderPage>(std::move(error));
    }
  }

  void start_order_history(KisAccount account, KisHistoryRange range, OrderPageHandler completion) {
    auto invalid = validate_account(account);
    if (
      !invalid &&
      (!is_valid_date(range.from_) || !is_valid_date(range.to_) || range.to_ < range.from_)
    ) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "invalid_date_range";
      error.message_ = "KIS order history requires a valid inclusive calendar range";
      invalid = std::move(error);
    }
    if (invalid) {
      complete_order_page(std::move(completion), Result<OrderPage>(std::move(*invalid)));
      return;
    }

    auto operation = std::make_shared<OrderHistoryOperation>(
      std::move(account), range, std::move(completion)
    );
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_order_page(
              std::move(operation->completion_), Result<OrderPage>(std::move(*token_error))
            );
            return;
          }
          self->request_order_history(operation);
        }
      )
    );
  }

  static network::HttpRequest make_order_history_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    const KisHistoryRange& range,
    std::string_view foreign_cursor,
    std::string_view domestic_cursor,
    bool continuation
  ) {
    network::HttpRequest request;
    request.method_ = http::verb::get;
    request.target_ =
      "/uapi/domestic-stock/v1/trading/inquire-daily-ccld"
      "?CANO=" +
      account.account_number_ + "&ACNT_PRDT_CD=" + account.product_code_ + "&INQR_STRT_DT=" +
      compact_date(range.from_) + "&INQR_END_DT=" + compact_date(range.to_) +
      "&SLL_BUY_DVSN_CD=00&PDNO=&ORD_GNO_BRNO=&ODNO=&CCLD_DVSN=00&INQR_DVSN=00"
      "&INQR_DVSN_1=&INQR_DVSN_3=00&EXCG_ID_DVSN_CD=" +
      (config.is_demo_ ? "KRX" : "ALL") + "&CTX_AREA_FK100=" +
      broker_detail::query_encode(foreign_cursor) + "&CTX_AREA_NK100=" +
      broker_detail::query_encode(domestic_cursor);
    request.headers_ = {
      {"authorization", "Bearer " + std::string(access_token)},
      {"appkey", config.key()},
      {"appsecret", config.secret()},
      {"tr_id", config.is_demo_ ? "VTTC0081R" : "TTTC0081R"},
      {"tr_cont", continuation ? "N" : ""},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
    return request;
  }

  void request_order_history(const std::shared_ptr<OrderHistoryOperation>& operation) {
    auto request = make_order_history_request(
      operation->account_,
      config_,
      token_.access_token_,
      operation->range_,
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
            self->handle_order_history_page(operation, std::move(result));
          }
        );
      }
    );
  }

  void handle_order_history_page(
    const std::shared_ptr<OrderHistoryOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      complete_order_page(
        std::move(operation->completion_), Result<OrderPage>(std::move(result.error()))
      );
      return;
    }

    const bool has_next = continuation_pending(result.value());
    std::string foreign_cursor;
    std::string domestic_cursor;
    auto page = parse_order_history_page(std::move(result), foreign_cursor, domestic_cursor);
    if (!page) {
      complete_order_page(
        std::move(operation->completion_), Result<OrderPage>(std::move(page.error()))
      );
      return;
    }

    auto pagination_error = advance_cursors(
      operation->page_count_,
      operation->foreign_cursor_,
      operation->domestic_cursor_,
      std::move(foreign_cursor),
      std::move(domestic_cursor),
      has_next,
      "KIS order-history continuation did not advance"
    );
    if (pagination_error) {
      complete_order_page(
        std::move(operation->completion_), Result<OrderPage>(std::move(*pagination_error))
      );
      return;
    }

    operation->page_.orders_.insert(
      operation->page_.orders_.end(),
      std::make_move_iterator(page.value().orders_.begin()),
      std::make_move_iterator(page.value().orders_.end())
    );
    if (has_next) {
      request_order_history(operation);
      return;
    }

    operation->page_.received_at_ = std::chrono::system_clock::now();
    complete_order_page(
      std::move(operation->completion_), Result<OrderPage>(std::move(operation->page_))
    );
  }

  static Result<OrderPage> parse_order_history_page(
    Result<network::HttpResponse> result,
    std::string& foreign_cursor,
    std::string& domestic_cursor
  ) {
    if (!result) {
      return Result<OrderPage>(std::move(result.error()));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<OrderPage>(broker_detail::http_error(result.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<OrderPage>(std::move(parse_error));
    }
    try {
      if (auto declared = kis_declared_error(*document)) {
        return Result<OrderPage>(std::move(*declared));
      }
      const auto rows = document->if_contains("output1");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: output1");
      }

      OrderPage page;
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("output1 item is not an object");
        }
        const auto& row = value.as_object();

        OrderSnapshot order;
        order.order_id_ = broker_detail::json_string(row, "odno");
        order.instrument_ = InstrumentId{
          Country::KR, Venue::COMBINED, broker_detail::json_string(row, "pdno")
        };
        order.name_ = broker_detail::json_string(row, "prdt_name");
        order.side_ = normalized_order_side(
          broker_detail::json_string(row, "sll_buy_dvsn_cd")
        );
        order.type_ = normalized_order_type(broker_detail::json_string(row, "ord_dvsn_cd"));
        order.time_in_force_ = order.type_ == OrderType::UNKNOWN ?
          TimeInForce::UNKNOWN :
          TimeInForce::DAY;
        order.quantity_ = Decimal64::parse(broker_detail::json_string(row, "ord_qty"));
        if (order.type_ == OrderType::LIMIT) {
          order.price_ = Decimal64::parse(broker_detail::json_string(row, "ord_unpr"));
        }
        order.currency_ = Currency::krw();
        order.ordered_at_ = korea_timestamp(
          broker_detail::json_string(row, "ord_dt"), broker_detail::json_string(row, "ord_tmd")
        );

        const auto filled = Decimal64::parse(broker_detail::json_string(row, "tot_ccld_qty"));
        const auto remaining = Decimal64::parse(broker_detail::json_string(row, "rmn_qty"));
        const auto rejected = Decimal64::parse(broker_detail::json_string(row, "rjct_qty"));
        const bool canceled = broker_detail::json_string(row, "cncl_yn") == "Y";

        order.execution_.filled_quantity_ = filled;
        order.execution_.filled_amount_ = Money{
          Decimal64::parse(broker_detail::json_string(row, "tot_ccld_amt")), Currency::krw()
        };
        if (const auto average = Decimal64::parse(broker_detail::json_string(row, "avg_prvs"));
            average.coefficient() > 0) {
          order.execution_.average_filled_price_ = average;
        }
        order.status_ =
          derive_order_status(order.quantity_, filled, remaining, rejected, canceled);
        page.orders_.push_back(std::move(order));
      }

      read_cursors(*document, foreign_cursor, domestic_cursor);
      return Result<OrderPage>(std::move(page));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_order_history_response";
      error.message_ = exception.what();
      return Result<OrderPage>(std::move(error));
    }
  }

  /**
   * Derives a normalized lifecycle state for one order-history row.
   *
   * KIS's daily order inquiry reports no status column, so the state is
   * reconstructed from the ordered, filled, remaining, and rejected quantities
   * together with the cancellation flag. This is the one place where the
   * adapter states something KIS did not, so it is isolated and directly
   * tested rather than inlined into the response loop.
   *
   * A cancelled or rejected order keeps its partial fills visible through
   * `OrderExecution`, so the terminal state is reported without discarding
   * what was executed before it.
   */
  static OrderStatus derive_order_status(
    const Decimal64& ordered,
    const Decimal64& filled,
    const Decimal64& remaining,
    const Decimal64& rejected,
    bool canceled
  ) {
    if (rejected.coefficient() > 0 && filled.coefficient() == 0) {
      return OrderStatus::REJECTED;
    }
    if (canceled) {
      return OrderStatus::CANCELED;
    }
    if (filled.coefficient() == 0) {
      return remaining.coefficient() > 0 ? OrderStatus::PENDING : OrderStatus::UNKNOWN;
    }
    return filled == ordered ? OrderStatus::FILLED : OrderStatus::PARTIAL_FILLED;
  }

  /** Maps a KIS `ORD_DVSN` code onto a normalized order type. */
  static OrderType normalized_order_type(std::string_view division) {
    if (division == "00") {
      return OrderType::LIMIT;
    }
    if (division == "01") {
      return OrderType::MARKET;
    }
    return OrderType::UNKNOWN;
  }

  /** Maps a KIS side code or rejects an invalid broker response. */
  static OrderSide normalized_order_side(std::string_view code) {
    if (code == "01") {
      return OrderSide::SELL;
    }
    if (code == "02") {
      return OrderSide::BUY;
    }
    throw std::runtime_error("invalid KIS order side code");
  }

  /** Returns whether the response header requests another continuation page. */
  static bool continuation_pending(const network::HttpResponse& response) {
    const auto continuation = response.base().find("tr_cont");
    return continuation != response.base().end() &&
      (continuation->value() == "M" || continuation->value() == "F");
  }

  /** Copies KIS's continuation cursors out of a response document. */
  static void read_cursors(
    const json::object& document,
    std::string& foreign_cursor,
    std::string& domestic_cursor
  ) {
    if (const auto value = document.if_contains("ctx_area_fk100"); value && value->is_string()) {
      foreign_cursor = value->as_string().c_str();
    }
    if (const auto value = document.if_contains("ctx_area_nk100"); value && value->is_string()) {
      domestic_cursor = value->as_string().c_str();
    }
  }

  /**
   * Advances continuation cursors, rejecting a continuation that cannot end.
   *
   * An empty, unchanged, or excessively long continuation would loop forever
   * against a misbehaving response, so each is reported as a protocol error.
   */
  static std::optional<BrokerError> advance_cursors(
    std::size_t& page_count,
    std::string& foreign_cursor,
    std::string& domestic_cursor,
    std::string next_foreign_cursor,
    std::string next_domestic_cursor,
    bool has_next,
    std::string_view message
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
        error.message_ = std::string(message);
        return error;
      }
    }
    ++page_count;
    if (has_next) {
      foreign_cursor = std::move(next_foreign_cursor);
      domestic_cursor = std::move(next_domestic_cursor);
    }
    return std::nullopt;
  }

  /** Formats a calendar date as KIS's `YYYYMMDD` inquiry bound. */
  static std::string compact_date(const Date& date) {
    char buffer[9];
    std::snprintf(
      buffer, sizeof(buffer), "%04d%02u%02u", date.year_, date.month_, date.day_
    );
    return std::string(buffer);
  }

  /**
   * Combines KIS's `YYYYMMDD` order date and `HHMMSS` order time.
   *
   * KIS's domestic trading endpoints report Korea Standard Time, so the fixed
   * `+09:00` offset is documented broker behaviour rather than an assumption
   * about the caller's locale.
   */
  static std::optional<std::chrono::system_clock::time_point> korea_timestamp(
    std::string_view date,
    std::string_view time
  ) {
    if (date.size() != 8 || time.size() != 6) {
      return std::nullopt;
    }
    std::string iso;
    iso.reserve(25);
    iso.append(date.substr(0, 4)).append("-").append(date.substr(4, 2)).append("-")
      .append(date.substr(6, 2)).append("T").append(time.substr(0, 2)).append(":")
      .append(time.substr(2, 2)).append(":").append(time.substr(4, 2)).append("+09:00");
    return parse_iso8601_timestamp(iso);
  }

  void complete_order_ack(OrderAckHandler handler, Result<KisOrderAck> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  void complete_open_orders(OpenOrderHandler handler, Result<KisOpenOrderPage> result) {
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
    return run<QuoteSnapshot>(
      [&](auto&& sink) { client_->async_quote(std::move(instrument), std::move(sink)); },
      "quote"
    );
  }

  /** Returns all normalized holdings for the selected KIS account. */
  Result<HoldingsSnapshot> holdings(KisAccount account) {
    return run<HoldingsSnapshot>(
      [&](auto&& sink) { client_->async_holdings(std::move(account), std::move(sink)); },
      "holdings"
    );
  }


  /**
   * Submits a new cash order and returns KIS's acknowledgement.
   *
   * A `TRANSPORT` error leaves the outcome indeterminate. The order is never
   * resubmitted automatically; reconcile through `open_orders()` or
   * `order_history()` before retrying.
   */
  Result<KisOrderAck> place_order(KisAccount account, OrderRequest order) {
    return run<KisOrderAck>(
      [&](auto&& sink) {
        client_->async_place_order(std::move(account), std::move(order), std::move(sink));
      },
      "order placement"
    );
  }

  /** Modifies the price or quantity of an existing order. */
  Result<KisOrderAck> replace_order(KisAccount account, KisAmendment amendment) {
    return run<KisOrderAck>(
      [&](auto&& sink) {
        client_->async_replace_order(std::move(account), std::move(amendment), std::move(sink));
      },
      "order modification"
    );
  }

  /** Cancels an existing order. */
  Result<KisOrderAck> cancel_order(KisAccount account, KisAmendment amendment) {
    return run<KisOrderAck>(
      [&](auto&& sink) {
        client_->async_cancel_order(std::move(account), std::move(amendment), std::move(sink));
      },
      "order cancellation"
    );
  }

  /** Returns every order that is still modifiable or cancellable. */
  Result<KisOpenOrderPage> open_orders(KisAccount account) {
    return run<KisOpenOrderPage>(
      [&](auto&& sink) { client_->async_open_orders(std::move(account), std::move(sink)); },
      "modifiable-order inquiry"
    );
  }

  /** Returns placed orders and their executions over an inclusive date range. */
  Result<OrderPage> order_history(KisAccount account, KisHistoryRange range) {
    return run<OrderPage>(
      [&](auto&& sink) {
        client_->async_order_history(std::move(account), range, std::move(sink));
      },
      "order-history inquiry"
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
  std::shared_ptr<KisClient> client_;
};

} // namespace algotradelib::kis
