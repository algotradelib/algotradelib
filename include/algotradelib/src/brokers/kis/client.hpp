#pragma once

#include "./config.hpp"
#include "../common.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <array>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
 *
 * The shape is all three normalized facts, not just the price basis, and the
 * venue with them. KIS re-sends `ORD_DVSN` on an amendment, and it composes
 * that one code from the three; naming only the basis made an amendment
 * re-shape the order it was amending -- an immediate-or-cancel order amended to
 * a new price was sent as a plain day order. `venue_` is needed for the same
 * reason the placement path needs it: the three venues publish different code
 * sets, so the same shape is a different code, or no code at all, depending on
 * where the order lives.
 */
struct KisAmendment {
  KisOrderRef order_;
  OrderType type_{OrderType::LIMIT};
  TimeInForce time_in_force_{TimeInForce::DAY};
  OrderSession session_{OrderSession::REGULAR};
  Venue venue_{Venue::UNSPECIFIED};
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

/**
 * Returns today's calendar date in Seoul.
 *
 * KIS dates every request and response in Korean local time, so "today" for a KIS
 * call is today there, not wherever the process happens to run.
 */
inline Date today_in_seoul() {
  const auto seoul = std::chrono::system_clock::now() + std::chrono::hours(9);
  const std::time_t raw = std::chrono::system_clock::to_time_t(
    std::chrono::time_point_cast<std::chrono::seconds>(seoul)
  );
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &raw);
#else
  gmtime_r(&raw, &parts);
#endif
  return Date{
    parts.tm_year + 1900,
    static_cast<unsigned>(parts.tm_mon + 1),
    static_cast<unsigned>(parts.tm_mday),
  };
}

/**
 * Which of KIS's two request-rate limits a declared failure reports.
 *
 * KIS meters requests in two independent layers and its documentation is explicit
 * that they must not be confused: "원장 언급이 없는 '초당 거래건수를 초과하였습니다.'
 * 메시지는 개인 유량 초과 시 발생하는 메시지로 착오 없으시길 바랍니다."
 *
 * The layer is preserved rather than collapsed into one category because it tells
 * a caller whose limit was reached: its own, or one it shares with every other
 * customer and can do nothing about.
 */
enum class KisRateLimitLayer {
  /** Not a rate limit. */
  NONE,
  /**
   * The ledger's own per-endpoint cap, shared across all customers.
   *
   * Documented for 주식잔고조회 and ETF 현재가 호가 as 초당 120 TPS "개인 고객 유량
   * 무관하게". The capacity is not this client's to spend, so no budget it could
   * configure would have avoided the refusal; the documented remedy is to retry.
   */
  LEDGER,
  /** This application key's own per-second allowance was exceeded. */
  PERSONAL,
};

/**
 * Classifies a declared KIS failure as one of the two rate-limit layers.
 *
 * Matching is by message code only: two integer-width string comparisons on a path
 * that is already an error, and nothing that depends on the broker's wording or
 * locale.
 *
 * `EGW00201` is the personal layer and `EGW00215` the ledger layer. Only
 * `EGW00215` appears in the API workbook; `EGW00201` is documented in KIS's
 * example repository as "초당 거래건수를 초과하였습니다", which is why that repository
 * is worth consulting where the workbook is silent.
 *
 * An unrecognized code stays unclassified and therefore reaches the caller as an
 * ordinary protocol failure. That is the safe direction: a missed classification
 * costs pacing, while a false one would make the library wait out a failure that
 * is not congestion at all.
 *
 * `EGW00317` is deliberately excluded. It reports a scheduled trading-control
 * window — 해외주식 상품기본정보 between 08:59 and 09:31 — during which the endpoint
 * is unavailable; treating it as congestion would misreport a closed window.
 */
inline KisRateLimitLayer kis_rate_limit_layer(std::string_view code) {
  if (code == "EGW00201") {
    return KisRateLimitLayer::PERSONAL;
  }
  return code == "EGW00215" ? KisRateLimitLayer::LEDGER : KisRateLimitLayer::NONE;
}

/**
 * Returns an error when an order carries something KIS cannot honour.
 *
 * KIS documents no client-supplied idempotency key for an individual customer.
 * `gt_uid` exists but is marked `[법인 전용]`, corporate only, so for everyone else
 * there is nothing for `OrderRequest::client_order_id_` to be written to.
 *
 * Dropping it silently is the dangerous option, not the convenient one. A
 * `TRANSPORT` error on an order POST leaves the outcome indeterminate, and a caller
 * who set a client order id has been taught by every broker that honours one that
 * resubmitting is the safe response to exactly that situation. At KIS it produces a
 * second live order. Refusing the field before the order is sent says plainly that
 * the guarantee the caller is relying on does not exist here.
 *
 * `OrderRequest::confirm_high_value_` is deliberately *not* refused. It has no KIS
 * counterpart either, but it gates an order rather than describing one: ignoring it
 * cannot make KIS transmit an order the caller did not describe, because the caller
 * was confirming that the order should proceed and it does.
 *
 * Both the client and the broker port call this, so the rule has one definition.
 */
inline std::optional<BrokerError> unsupported_client_order_id(const OrderRequest& order) {
  if (!order.client_order_id_) {
    return std::nullopt;
  }
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "idempotency_key_unsupported";
  error.message_ =
    "KIS offers no client-supplied idempotency key for individual customers; clear "
    "client_order_id_ and reconcile through an order inquiry after a transport failure";
  return error;
}

/**
 * Returns an error when KIS does not publish bars at `interval`.
 *
 * KIS publishes all of them — minute, daily, weekly, monthly, and yearly — so this
 * always accepts. It exists so that the port asks both brokers the same question
 * in the same way rather than special-casing the one that happens to say yes.
 */
inline std::optional<BrokerError> unsupported_bar_interval(BarInterval) { return std::nullopt; }

/** Persistent asynchronous KIS client with coalesced OAuth refresh. */
class KisClient : public std::enable_shared_from_this<KisClient> {
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

  /** Completion handler for one trading calendar. */
  using CalendarHandler = net::any_completion_handler<void(Result<TradingCalendar>)>;

  /** Completion handler for one instrument's reference data. */
  using InstrumentHandler = net::any_completion_handler<void(Result<Instrument>)>;

  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;
  /** Completion handler for one normalized buying-power reading. */
  using BuyingPowerHandler = net::any_completion_handler<void(Result<BuyingPower>)>;

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

  /**
   * Requests bars, walking KIS's date and time inputs until `count_` is satisfied.
   *
   * KIS answers this question from three endpoints — period bars, today's minute
   * bars, and past days' minute bars — with different page sizes and no cursor of
   * any kind. Which one is called is chosen from the interval and the requested
   * upper bound; a caller sees one verb.
   *
   * A minute request does not cross a trading day. KIS pages minutes by time
   * within a day, and stepping to the previous *trading* day needs a calendar this
   * library does not have yet, so a series may be shorter than `count_` and
   * carries `next_cursor_` for the caller to continue from deliberately.
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

  /** Requests static reference data for one instrument. */
  template <typename CompletionToken>
  auto async_instrument(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<Instrument>)>(
      [self = shared_from_this(), instrument = std::move(instrument)](auto&& handler) mutable {
        auto completion = InstrumentHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, instrument = std::move(instrument), completion = std::move(completion)]() mutable {
            self->start_instrument(std::move(instrument), std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Requests the trading calendar from `from` onwards, and caches it.
   *
   * **KIS asks that this endpoint be called at most once a day.** Its documentation
   * is explicit: "(★중요) 국내휴장일조회 … 당사 원장서비스와 연관되어 있어 단시간 내 다수
   * 호출시 서비스에 영향을 줄 수 있어 가급적 1일 1회 호출 부탁드립니다." The cost of
   * ignoring that falls on the broker's ledger service, not on this client, so it
   * cannot be discovered by being rate limited.
   *
   * So the answer is cached for the life of the client, and a second call for a
   * date the cache already covers is answered from it without a request. Pass
   * `refresh` to fetch again deliberately.
   */
  template <typename CompletionToken>
  auto async_calendar(Date from, bool refresh, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<TradingCalendar>)>(
      [self = shared_from_this(), from, refresh](auto&& handler) mutable {
        auto completion = CalendarHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, from, refresh, completion = std::move(completion)]() mutable {
            self->start_calendar(from, refresh, std::move(completion));
          }
        );
      },
      token
    );
  }

  /**
   * Requests today's upper and lower price bound for one instrument.
   *
   * KIS reports both on its current-price inquiry rather than on an endpoint of
   * their own, so this reads `stck_mxpr` and `stck_llam` from there.
   */
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
              "/uapi/domestic-stock/v1/quotations/inquire-price",
              "FHKST01010100",
              std::move(completion),
              parse_price_limits
            );
          }
        );
      },
      token
    );
  }

  /**
   * Requests recent executions for one instrument.
   *
   * KIS returns a fixed window on this endpoint rather than a requested count, so
   * `count` trims what arrived instead of asking for less.
   */
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
            self->start_market_read(
              std::move(instrument),
              "/uapi/domestic-stock/v1/quotations/inquire-ccnl",
              "FHKST01010300",
              std::move(completion),
              [count](InstrumentId subject, Result<network::HttpResponse> response) {
                return parse_trades(std::move(subject), std::move(response), count);
              }
            );
          }
        );
      },
      token
    );
  }

  /** Requests and normalizes the ten resting levels on each side of the book. */
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

  /**
   * Requests what the account may spend, in won.
   *
   * KIS answers this per instrument and per order division when asked to, and the same endpoint
   * answers the **account-level amount alone** when the symbol and the unit price are left blank --
   * which is the question the port asks, and the only one both brokers answer the same way. The
   * per-instrument quantities stay reachable through this client for a caller that wants them.
   */
  template <typename CompletionToken>
  auto async_buying_power(Currency currency, KisAccount account, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<BuyingPower>)>(
      [self = shared_from_this(), currency, account = std::move(account)](auto&& handler) mutable {
        auto completion = BuyingPowerHandler(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, currency, account = std::move(account), completion = std::move(completion)]()
            mutable {
            self->start_buying_power(currency, std::move(account), std::move(completion));
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
  friend struct KisClientTestAccess;

  net::strand<net::any_io_executor> strand_;
  KisConfig config_;
  std::shared_ptr<network::AsyncHttpClient> http_;
  KisRest token_;
  TradingCalendar calendar_;
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

  /** One bar request spanning however many KIS pages it needs. */
  struct BarOperation {
    BarQuery query_;
    std::vector<Bar> descending_;
    /** Upper bound for the next page, as a KST calendar date and time. */
    Date date_{};
    std::string time_;
    int pages_{0};
    bool minutes_{false};
    BarsHandler completion_;

    BarOperation(BarQuery query, BarsHandler completion)
    : query_(std::move(query)), completion_(std::move(completion)) {}
  };

  /** Bars KIS returns per page, per endpoint. */
  static constexpr int period_page_limit_ = 100;
  static constexpr int today_minute_page_limit_ = 30;
  static constexpr int daily_minute_page_limit_ = 120;

  /** Most pages one bar request may consume, for the reason paging is bounded. */
  static constexpr int bar_page_budget_ = 50;

  /** Validates a bar request and starts walking KIS's inputs. */
  void start_bars(BarQuery query, BarsHandler completion) {
    if (auto invalid = validate_bar_query(query)) {
      deliver(std::move(completion), Result<BarSeries>(std::move(*invalid)));
      return;
    }

    auto operation = std::make_shared<BarOperation>(std::move(query), std::move(completion));
    operation->minutes_ = operation->query_.interval_ == BarInterval::ONE_MINUTE;
    const auto bound = operation->query_.before_ ?
      *operation->query_.before_ :
      std::chrono::system_clock::now();
    korea_calendar(bound, operation->date_, operation->time_);

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

  /** Returns an error when KIS cannot answer the request as described. */
  static std::optional<BrokerError> validate_bar_query(const BarQuery& query) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (query.instrument_.country_ != Country::KR) {
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS bars currently cover Korean equities only";
      return error;
    }
    if (!valid_domestic_symbol(query.instrument_.symbol_)) {
      error.broker_code_ = "invalid_instrument";
      error.message_ =
        "KIS bar symbol must contain six letters or digits, or seven for an ETN";
      return error;
    }
    if (query.count_ < 1) {
      error.broker_code_ = "invalid_count";
      error.message_ = "bar request must ask for at least one bar";
      return error;
    }
    return std::nullopt;
  }

  /** Returns KIS's `FID_PERIOD_DIV_CODE` for a period interval. */
  static const char* period_code(BarInterval interval) {
    switch (interval) {
      case BarInterval::ONE_WEEK:
        return "W";
      case BarInterval::ONE_MONTH:
        return "M";
      case BarInterval::ONE_YEAR:
        return "Y";
      case BarInterval::ONE_DAY:
      case BarInterval::ONE_MINUTE:
        break;
    }
    return "D";
  }

  /**
   * Returns the fewest calendar days one bar of `interval` can span.
   *
   * A period request asks KIS for a *date range* and KIS answers with at most 100
   * rows, so the range has to be sized to hold about one page of bars. Sizing it in
   * bars is not possible and sizing it as a fixed number of days is wrong: 100
   * calendar days is at most 100 daily bars, but only about fourteen weekly bars,
   * three monthly ones, and a quarter of a yearly one. A fixed span would have made
   * a request for a hundred monthly bars take thirty pages and a hundred yearly
   * bars exceed the page budget outright, returning a fraction of what was asked
   * for and reporting nothing wrong.
   *
   * The multiplier is the *shortest* a bar of that interval can be, so a range
   * never holds more than a page and the row cap cannot truncate it from an end
   * this code has not established.
   */
  static constexpr int shortest_days_per_bar(BarInterval interval) {
    switch (interval) {
      case BarInterval::ONE_WEEK:
        return 7;
      case BarInterval::ONE_MONTH:
        return 28;
      case BarInterval::ONE_YEAR:
        return 365;
      case BarInterval::ONE_DAY:
      case BarInterval::ONE_MINUTE:
        break;
    }
    return 1;
  }

  /** Splits an instant into the KST calendar date and `HHMMSS` KIS asks for. */
  static void korea_calendar(
    std::chrono::system_clock::time_point value,
    Date& date,
    std::string& time
  ) {
    const auto seoul = value + std::chrono::hours(9);
    const std::time_t raw = std::chrono::system_clock::to_time_t(
      std::chrono::time_point_cast<std::chrono::seconds>(seoul)
    );
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &raw);
#else
    gmtime_r(&raw, &parts);
#endif
    date.year_ = parts.tm_year + 1900;
    date.month_ = static_cast<unsigned>(parts.tm_mon + 1);
    date.day_ = static_cast<unsigned>(parts.tm_mday);
    char buffer[7];
    std::snprintf(buffer, sizeof(buffer), "%02d%02d%02d", parts.tm_hour, parts.tm_min, parts.tm_sec);
    time.assign(buffer, 6);
  }

  /** Returns the calendar day before `date`, without a calendar of trading days. */
  static Date previous_day(const Date& date) {
    Date earlier = date;
    if (earlier.day_ > 1) {
      --earlier.day_;
      return earlier;
    }
    if (earlier.month_ > 1) {
      --earlier.month_;
    } else {
      earlier.month_ = 12;
      --earlier.year_;
    }
    static constexpr unsigned days_per_month[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    unsigned days = days_per_month[earlier.month_ - 1];
    const bool leap =
      earlier.year_ % 4 == 0 && (earlier.year_ % 100 != 0 || earlier.year_ % 400 == 0);
    if (earlier.month_ == 2 && leap) {
      ++days;
    }
    earlier.day_ = days;
    return earlier;
  }

  /** Sends the next page, choosing the endpoint the interval and bound imply. */
  void request_bars(const std::shared_ptr<BarOperation>& operation) {
    network::HttpRequest request;
    // Bars are the traffic a trading reserve exists to hold back.
    request.group_ = network::RateLimitGroup::MARKET_DATA;
    request.method_ = http::verb::get;

    std::string transaction;
    if (!operation->minutes_) {
      // A period request selects bars by date range, oldest bound included. The
      // range has to be sized in calendar days, and how many calendar days a page
      // of bars spans depends entirely on the interval.
      Date from = operation->date_;
      const int span = period_page_limit_ * shortest_days_per_bar(operation->query_.interval_);
      for (int step = 0; step < span && from.year_ > 1; ++step) {
        from = previous_day(from);
      }
      request.target_ =
        "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice"
        "?FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" +
        broker_detail::query_encode(operation->query_.instrument_.symbol_) +
        "&FID_INPUT_DATE_1=" + compact_date(from) +
        "&FID_INPUT_DATE_2=" + compact_date(operation->date_) +
        "&FID_PERIOD_DIV_CODE=" + period_code(operation->query_.interval_) +
        // KIS spells this the other way round from every reading of its name:
        // `0` asks for adjusted prices and `1` for raw ones.
        "&FID_ORG_ADJ_PRC=" + std::string(operation->query_.adjusted_ ? "0" : "1");
      transaction = "FHKST03010100";
    } else if (today_in_korea(operation->date_)) {
      request.target_ =
        "/uapi/domestic-stock/v1/quotations/inquire-time-itemchartprice"
        "?FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" +
        broker_detail::query_encode(operation->query_.instrument_.symbol_) +
        "&FID_INPUT_HOUR_1=" + operation->time_ + "&FID_PW_DATA_INCU_YN=Y&FID_ETC_CLS_CODE=";
      transaction = "FHKST03010200";
    } else {
      request.target_ =
        "/uapi/domestic-stock/v1/quotations/inquire-time-dailychartprice"
        "?FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" +
        broker_detail::query_encode(operation->query_.instrument_.symbol_) +
        "&FID_INPUT_DATE_1=" + compact_date(operation->date_) +
        "&FID_INPUT_HOUR_1=" + operation->time_ + "&FID_PW_DATA_INCU_YN=Y&FID_FAKE_TICK_INCU_YN=N";
      transaction = "FHKST03010230";
    }

    request.headers_ = {
      {"authorization", "Bearer " + token_.access_token_},
      {"appkey", config_.key()},
      {"appsecret", config_.secret()},
      {"tr_id", transaction},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };

    ++operation->pages_;
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), operation](Result<network::HttpResponse> result) mutable {
        self->handle_bars_page(operation, std::move(result));
      }
    );
  }

  /** Returns whether `date` is today in Seoul, which decides the minute endpoint. */
  static bool today_in_korea(const Date& date) {
    Date today{};
    std::string ignored;
    korea_calendar(std::chrono::system_clock::now(), today, ignored);
    return today == date;
  }

  /** Accumulates one page and either continues or finishes the request. */
  void handle_bars_page(
    const std::shared_ptr<BarOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    const auto before = operation->descending_.size();
    if (auto failure = parse_bars_page(
          std::move(result), operation->query_, operation->minutes_, operation->descending_
        )) {
      deliver(std::move(operation->completion_), Result<BarSeries>(std::move(*failure)));
      return;
    }

    const bool empty_page = operation->descending_.size() == before;
    const int collected = static_cast<int>(operation->descending_.size());
    const bool satisfied = collected >= operation->query_.count_;
    const bool spent = operation->pages_ >= bar_page_budget_;
    if (satisfied || empty_page || spent) {
      finish_bars(operation);
      return;
    }

    // Step the bound to just before the oldest bar this page returned. Within a
    // session that walks backwards a page at a time; once the session start is
    // passed KIS returns nothing and the empty-page guard above ends the request.
    // The date check below is the second line of defence, for a bound that crosses
    // midnight: a minute request never steps to another calendar day, because
    // choosing the previous *trading* day needs a calendar this slice has not built.
    const auto& oldest = operation->descending_.back();
    Date date{};
    std::string time;
    korea_calendar(oldest.timestamp_ - std::chrono::seconds(1), date, time);
    if (operation->minutes_ && !(date == operation->date_)) {
      finish_bars(operation);
      return;
    }
    operation->date_ = date;
    operation->time_ = time;
    request_bars(operation);
  }

  /**
   * Parses one page into `into`, or returns the error that ends the request.
   *
   * Takes the accumulation by reference rather than an operation, so the
   * normalization is a pure function of a response and can be tested as one.
   */
  static std::optional<BrokerError> parse_bars_page(
    Result<network::HttpResponse> result,
    const BarQuery& query,
    bool minutes,
    std::vector<Bar>& into
  ) {
    if (!result) {
      return std::move(result.error());
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return broker_detail::http_error(result.value());
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return parse_error;
    }
    try {
      if (auto declared = kis_declared_error(*document)) {
        return declared;
      }
      const auto rows = document->if_contains("output2");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: output2");
      }
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("bar entry is not an object");
        }
        const auto& row = value.as_object();
        // KIS pads a page with blank rows rather than shortening it.
        if (broker_detail::json_string(row, "stck_bsop_date").empty()) {
          continue;
        }
        into.push_back(parse_bar(row, query, minutes));
      }
      return std::nullopt;
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_bar_response";
      error.message_ = exception.what();
      return error;
    }
  }

  /**
   * Normalizes one KIS bar.
   *
   * A period bar is reported by its trading date, so its timestamp marks the
   * period's open. A minute bar carries `stck_cntg_hour`, the execution time the
   * bar was aggregated to, so it marks the close.
   *
   * `mod_yn` marks a trading day on which nothing traded, so the bar has no open.
   * An absent open is preserved as absent rather than becoming zero or the
   * previous close, which is the only honest reading of a day with no trade.
   */
  static Bar parse_bar(const json::object& row, const BarQuery& query, bool minutes) {
    static_cast<void>(query);
    Bar bar;
    const auto date = broker_detail::json_string(row, "stck_bsop_date");
    const auto time = minutes ? broker_detail::json_string(row, "stck_cntg_hour") : "000000";
    const auto timestamp = korea_timestamp(date, time);
    if (!timestamp) {
      throw std::runtime_error("invalid bar timestamp");
    }
    bar.timestamp_ = *timestamp;
    bar.high_ = Decimal64::parse(broker_detail::json_string(row, "stck_hgpr"));
    bar.low_ = Decimal64::parse(broker_detail::json_string(row, "stck_lwpr"));
    bar.close_ = Decimal64::parse(
      broker_detail::json_string(row, minutes ? "stck_prpr" : "stck_clpr")
    );
    bar.volume_ = Decimal64::parse(
      broker_detail::json_string(row, minutes ? "cntg_vol" : "acml_vol")
    );
    if (const auto value = row.if_contains("acml_tr_pbmn"); value && value->is_string()) {
      const std::string traded(value->as_string());
      if (!traded.empty()) {
        bar.traded_value_ = Decimal64::parse(traded);
      }
    }
    const auto unchanged = row.if_contains("mod_yn");
    const bool no_trade =
      unchanged && unchanged->is_string() && std::string_view(unchanged->as_string()) == "Y";
    if (!no_trade) {
      bar.open_ = Decimal64::parse(broker_detail::json_string(row, "stck_oprc"));
    }
    return bar;
  }

  /** Reverses the accumulation, trims to the requested count, and completes. */
  void finish_bars(const std::shared_ptr<BarOperation>& operation) {
    BarSeries series;
    series.instrument_ = operation->query_.instrument_;
    series.interval_ = operation->query_.interval_;
    // A period bar is reported by its trading date, so its timestamp marks the
    // period's open; a minute bar carries the execution time it aggregated to.
    series.stamp_ = operation->minutes_ ? BarStamp::CLOSE : BarStamp::OPEN;
    series.currency_ = Currency::krw();
    series.adjusted_ = operation->query_.adjusted_;
    series.received_at_ = std::chrono::system_clock::now();

    auto& bars = operation->descending_;
    if (static_cast<int>(bars.size()) > operation->query_.count_) {
      bars.resize(static_cast<std::size_t>(operation->query_.count_));
    }
    series.bars_.assign(bars.rbegin(), bars.rend());
    if (!series.bars_.empty() && static_cast<int>(series.bars_.size()) < operation->query_.count_) {
      series.next_cursor_ = format_iso8601_utc(series.bars_.front().timestamp_);
    }
    deliver(std::move(operation->completion_), Result<BarSeries>(std::move(series)));
  }

  /**
   * Requests reference data, which KIS addresses differently from market data.
   *
   * `search-stock-info` takes a product-type code and a product number rather than
   * the market division every quotation endpoint takes, so it does not go through
   * the shared market-data helper.
   */
  void start_instrument(InstrumentId instrument, InstrumentHandler completion) {
    if (instrument.country_ != Country::KR || !valid_domestic_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS reference data covers Korean equities addressed by product number";
      deliver(std::move(completion), Result<Instrument>(std::move(error)));
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
            self->deliver(std::move(completion), Result<Instrument>(std::move(*token_error)));
            return;
          }
          network::HttpRequest request;
          // Reference data is the lowest-priority traffic there is.
          request.group_ = network::RateLimitGroup::REFERENCE;
          request.method_ = http::verb::get;
          // 300 covers 주식, ETF, ETN, and ELW, which is the whole equity universe
          // this library addresses.
          request.target_ =
            "/uapi/domestic-stock/v1/quotations/search-stock-info?PRDT_TYPE_CD=300&PDNO=" +
            broker_detail::query_encode(instrument.symbol_);
          request.headers_ = {
            {"authorization", "Bearer " + self->token_.access_token_},
            {"appkey", self->config_.key()},
            {"appsecret", self->config_.secret()},
            {"tr_id", "CTPF1002R"},
            {"custtype", "P"},
            {"content-type", "application/json; charset=utf-8"},
          };
          self->http_->async_request(
            std::move(request),
            [self, instrument = std::move(instrument), completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse_instrument(std::move(instrument), std::move(result))
              );
            }
          );
        }
      )
    );
  }

  /**
   * Normalizes KIS's reference data for one instrument.
   *
   * The identity comes from the instrument the caller asked about, not from `pdno`:
   * KIS returns that zero-padded and prefixed — `00000A000660` for `000660` — so
   * parsing a symbol back out of it would be inventing a rule KIS has not stated.
   *
   * KIS reports a listing date per market rather than one date, and blanks the ones
   * that do not apply, so the date is taken from the market the instrument is listed
   * on. It publishes no listing *status* at all, so `status_` stays `UNKNOWN` rather
   * than being inferred from the presence of a delisting date — a future delisting
   * date belongs to an instrument that is still trading.
   *
   * `price_rule_` is the library's own grid, because no broker publishes tick size.
   */
  static Result<Instrument> parse_instrument(
    InstrumentId requested,
    Result<network::HttpResponse> result
  ) {
    auto document = market_document(std::move(result));
    if (!document) {
      return Result<Instrument>(std::move(document.error()));
    }
    try {
      const auto output = document.value().if_contains("output");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output");
      }
      const auto& row = output->as_object();

      Instrument instrument;
      instrument.id_ = std::move(requested);
      instrument.name_ = broker_detail::json_string(row, "prdt_name");
      instrument.english_name_ = broker_detail::json_string(row, "prdt_eng_name");
      // KIS's 표준상품번호 is the KRX standard code, which is also the ISIN. KIS does not
      // say so in as many words, but it documents the field as twelve characters for
      // both domestic and foreign instruments, which is the ISIN's length and not the
      // length of any domestic-only code. Confirmed in review.
      instrument.isin_ = broker_detail::json_string(row, "std_pdno");
      const auto market = broker_detail::json_string(row, "mket_id_cd");
      instrument.segment_ = kis_market_segment(market);
      instrument.type_ =
        kis_security_type(broker_detail::json_string(row, "scty_grp_id_cd"));
      instrument.currency_ = Currency::krw();
      instrument.shares_outstanding_ = positive_decimal(row, "lstg_stqt");
      instrument.listed_on_ = compact_date_field(row, listing_date_field(market));
      instrument.delisted_on_ = compact_date_field(row, delisting_date_field(market));
      instrument.price_rule_ =
        price_rule_for(instrument.id_.country_, instrument.type_);
      return Result<Instrument>(std::move(instrument));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_reference_response";
      error.message_ = exception.what();
      return Result<Instrument>(std::move(error));
    }
  }

  /** Maps KIS's market identifier, preserving one this library has not normalized. */
  static MarketSegment kis_market_segment(std::string_view code) {
    if (code == "STK") {
      return market_segment("KOSPI");
    }
    if (code == "KSQ") {
      return market_segment("KOSDAQ");
    }
    // KONEX has no normalized segment, so its own code is preserved instead of
    // being collapsed into one of the two that do.
    return market_segment(code);
  }

  /**
   * Maps KIS's security group to a normalized type.
   *
   * KIS's group covers instruments far beyond equities — futures, options, gold,
   * ship-investment companies — and the ones with no normalized counterpart stay
   * `UNKNOWN` rather than being forced into a nearby value. An ELW is the notable
   * one: it is a warrant, but not the 신주인수권 Toss's `STOCK_WARRANTS` means.
   */
  static SecurityType kis_security_type(std::string_view group) {
    if (group == "ST" || group == "KN") {
      return SecurityType::STOCK;
    }
    if (group == "FS") {
      return SecurityType::FOREIGN_STOCK;
    }
    if (group == "DR") {
      return SecurityType::DEPOSITARY_RECEIPT;
    }
    if (group == "IF") {
      return SecurityType::INFRASTRUCTURE_FUND;
    }
    if (group == "RT") {
      return SecurityType::REIT;
    }
    if (group == "EF") {
      return SecurityType::ETF;
    }
    if (group == "FE") {
      return SecurityType::FOREIGN_ETF;
    }
    if (group == "EN") {
      return SecurityType::ETN;
    }
    if (group == "SW" || group == "SR") {
      return SecurityType::STOCK_WARRANT;
    }
    return SecurityType::UNKNOWN;
  }

  /** Returns which listing-date column applies to `market`. */
  static const char* listing_date_field(std::string_view market) {
    if (market == "KSQ") {
      return "kosdaq_mket_lstg_dt";
    }
    if (market == "STK") {
      return "scts_mket_lstg_dt";
    }
    return "frbd_mket_lstg_dt";
  }

  /** Returns which delisting-date column applies to `market`. */
  static const char* delisting_date_field(std::string_view market) {
    if (market == "KSQ") {
      return "kosdaq_mket_lstg_abol_dt";
    }
    if (market == "STK") {
      return "scts_mket_lstg_abol_dt";
    }
    return "frbd_mket_lstg_abol_dt";
  }

  /** Reads a `YYYYMMDD` field that KIS blanks when it does not apply. */
  static std::optional<Date> compact_date_field(
    const json::object& row,
    std::string_view key
  ) {
    const auto value = row.if_contains(key);
    if (!value || !value->is_string()) {
      return std::nullopt;
    }
    const std::string text(value->as_string());
    if (text.size() != 8) {
      return std::nullopt;
    }
    Date date{
      std::stoi(text.substr(0, 4)),
      static_cast<unsigned>(std::stoi(text.substr(4, 2))),
      static_cast<unsigned>(std::stoi(text.substr(6, 2))),
    };
    return is_valid_date(date) ? std::optional<Date>(date) : std::nullopt;
  }

  /** One calendar request spanning however many continuation pages it needs. */
  struct CalendarOperation {
    Date from_{};
    TradingCalendar calendar_;
    std::string search_cursor_;
    std::string next_cursor_;
    int pages_{0};
    CalendarHandler completion_;

    CalendarOperation(Date from, CalendarHandler completion)
    : from_(from), completion_(std::move(completion)) {}
  };

  /** Most continuation pages one calendar request may consume. */
  static constexpr int calendar_page_budget_ = 20;

  /**
   * Answers from the cache when it covers `from`, and otherwise fetches once.
   *
   * The cache exists because of the broker's once-a-day request, not as an
   * optimization, so it is consulted before anything else and is only bypassed when
   * the caller asks for a refresh explicitly.
   */
  void start_calendar(Date from, bool refresh, CalendarHandler completion) {
    if (!is_valid_date(from)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_date";
      error.message_ = "calendar request requires a valid Gregorian date";
      deliver(std::move(completion), Result<TradingCalendar>(std::move(error)));
      return;
    }
    if (!refresh && calendar_covers(from)) {
      deliver(std::move(completion), Result<TradingCalendar>(calendar_));
      return;
    }

    auto operation = std::make_shared<CalendarOperation>(from, std::move(completion));
    operation->calendar_.country_ = Country::KR;
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(operation->completion_), Result<TradingCalendar>(std::move(*token_error))
            );
            return;
          }
          self->request_calendar(operation);
        }
      )
    );
  }

  /** Returns whether the cached calendar already answers for `from` onwards. */
  bool calendar_covers(const Date& from) const {
    const auto covered = calendar_.covered();
    return covered && !(from < covered->first) && !(covered->second < from);
  }

  /** Sends one calendar page, continuing from the cursors KIS returned. */
  void request_calendar(const std::shared_ptr<CalendarOperation>& operation) {
    network::HttpRequest request;
    // The calendar is reference data, the lowest-priority traffic there is.
    request.group_ = network::RateLimitGroup::REFERENCE;
    request.method_ = http::verb::get;
    request.target_ =
      "/uapi/domestic-stock/v1/quotations/chk-holiday?BASS_DT=" +
      compact_date(operation->from_) +
      "&CTX_AREA_NK=" + broker_detail::query_encode(operation->next_cursor_) +
      "&CTX_AREA_FK=" + broker_detail::query_encode(operation->search_cursor_);
    request.headers_ = {
      {"authorization", "Bearer " + token_.access_token_},
      {"appkey", config_.key()},
      {"appsecret", config_.secret()},
      {"tr_id", "CTCA0903R"},
      {"tr_cont", operation->pages_ == 0 ? "" : "N"},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };

    ++operation->pages_;
    http_->async_request(
      std::move(request),
      [self = shared_from_this(), operation](Result<network::HttpResponse> result) mutable {
        self->handle_calendar_page(operation, std::move(result));
      }
    );
  }

  /** Accumulates one page and either continues or finishes and caches. */
  void handle_calendar_page(
    const std::shared_ptr<CalendarOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    const auto before = operation->calendar_.days_.size();
    const bool more = result && continuation_pending(result.value());
    std::string search = operation->search_cursor_;
    std::string next = operation->next_cursor_;

    if (auto failure = parse_calendar_page(std::move(result), operation->calendar_, search, next)) {
      deliver(
        std::move(operation->completion_), Result<TradingCalendar>(std::move(*failure))
      );
      return;
    }

    const bool empty_page = operation->calendar_.days_.size() == before;
    const bool repeated = next == operation->next_cursor_ && search == operation->search_cursor_;
    if (!more || empty_page || repeated || operation->pages_ >= calendar_page_budget_) {
      operation->calendar_.received_at_ = std::chrono::system_clock::now();
      calendar_ = operation->calendar_;
      deliver(std::move(operation->completion_), Result<TradingCalendar>(calendar_));
      return;
    }

    operation->search_cursor_ = std::move(search);
    operation->next_cursor_ = std::move(next);
    request_calendar(operation);
  }

  /**
   * Parses one calendar page.
   *
   * KIS reports four related flags per date — a banking business day, a securities
   * business day, a market-open day, and a settlement day. Only `opnd_yn` is
   * normalized, because it is the one KIS itself directs callers to use before
   * placing an order: "주문을 넣고자 할 경우 개장일여부(opnd_yn)를 사용". The other three
   * stay unread rather than being folded into one meaning.
   */
  static std::optional<BrokerError> parse_calendar_page(
    Result<network::HttpResponse> result,
    TradingCalendar& calendar,
    std::string& search_cursor,
    std::string& next_cursor
  ) {
    auto document = market_document(std::move(result));
    if (!document) {
      return std::move(document.error());
    }
    try {
      const auto rows = document.value().if_contains("output");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: output");
      }
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("calendar entry is not an object");
        }
        const auto& row = value.as_object();
        const auto date = broker_detail::json_string(row, "bass_dt");
        if (date.size() != 8) {
          continue;
        }
        TradingDay day;
        day.date_ = Date{
          std::stoi(date.substr(0, 4)),
          static_cast<unsigned>(std::stoi(date.substr(4, 2))),
          static_cast<unsigned>(std::stoi(date.substr(6, 2))),
        };
        day.open_ = broker_detail::json_string(row, "opnd_yn") == "Y";
        calendar.days_.push_back(day);
      }
      read_calendar_cursors(document.value(), search_cursor, next_cursor);
      return std::nullopt;
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_calendar_response";
      error.message_ = exception.what();
      return error;
    }
  }

  /** Reads the calendar continuation cursors, which KIS names without the 100 suffix. */
  static void read_calendar_cursors(
    const json::object& document,
    std::string& search_cursor,
    std::string& next_cursor
  ) {
    if (const auto value = document.if_contains("ctx_area_fk"); value && value->is_string()) {
      search_cursor = std::string(value->as_string());
    }
    if (const auto value = document.if_contains("ctx_area_nk"); value && value->is_string()) {
      next_cursor = std::string(value->as_string());
    }
  }

  /**
   * Issues one authenticated market-data GET for a single instrument.
   *
   * The single-instrument reads differ only in path, transaction id, and parser, so
   * the validation, token handling, and traffic class live here once.
   */
  template <typename Handler, typename Parse>
  void start_market_read(
    InstrumentId instrument,
    std::string path,
    std::string transaction,
    Handler completion,
    Parse parse
  ) {
    using ResultType = decltype(parse(InstrumentId{}, Result<network::HttpResponse>(BrokerError{})));
    if (instrument.country_ != Country::KR || !valid_domestic_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS market data covers Korean equities addressed by product number";
      deliver(std::move(completion), ResultType(std::move(error)));
      return;
    }

    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(),
          instrument = std::move(instrument),
          path = std::move(path),
          transaction = std::move(transaction),
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
          request.target_ = path + "?FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" +
            broker_detail::query_encode(instrument.symbol_);
          request.headers_ = {
            {"authorization", "Bearer " + self->token_.access_token_},
            {"appkey", self->config_.key()},
            {"appsecret", self->config_.secret()},
            {"tr_id", transaction},
            {"custtype", "P"},
            {"content-type", "application/json; charset=utf-8"},
          };
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
   * Returns a market-data response's declared payload, or the error ending it.
   *
   * Applies the HTTP status check, the JSON parse, and KIS's own `rt_cd` before a
   * parser looks at any field, so a declared failure is never read as data.
   */
  static Result<json::object> market_document(Result<network::HttpResponse> response) {
    if (!response) {
      return Result<json::object>(std::move(response.error()));
    }
    if (response.value().result_int() < 200 || response.value().result_int() >= 300) {
      return Result<json::object>(broker_detail::http_error(response.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(response.value().body(), parse_error);
    if (!document) {
      return Result<json::object>(std::move(parse_error));
    }
    try {
      if (auto declared = kis_declared_error(*document)) {
        return Result<json::object>(std::move(*declared));
      }
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_market_response";
      error.message_ = exception.what();
      return Result<json::object>(std::move(error));
    }
    return Result<json::object>(*document);
  }

  /**
   * Normalizes KIS's price bounds from its current-price inquiry.
   *
   * KRX sets a limit on every listed equity, so both bounds are expected here; a
   * blank or zero bound is reported as absent rather than as a bound of nothing.
   */
  static Result<PriceLimits> parse_price_limits(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    static_cast<void>(instrument);
    auto document = market_document(std::move(result));
    if (!document) {
      return Result<PriceLimits>(std::move(document.error()));
    }
    try {
      const auto output = document.value().if_contains("output");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output");
      }
      const auto& row = output->as_object();
      PriceLimits limits;
      limits.currency_ = Currency::krw();
      limits.upper_ = positive_decimal(row, "stck_mxpr");
      limits.lower_ = positive_decimal(row, "stck_llam");
      return Result<PriceLimits>(std::move(limits));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_price_limit_response";
      error.message_ = exception.what();
      return Result<PriceLimits>(std::move(error));
    }
  }

  /** Reads a decimal that KIS leaves blank or zero when it has no value. */
  static std::optional<Decimal64> positive_decimal(
    const json::object& row,
    std::string_view key
  ) {
    const auto value = row.if_contains(key);
    if (!value || !value->is_string()) {
      return std::nullopt;
    }
    const std::string text(value->as_string());
    if (text.empty()) {
      return std::nullopt;
    }
    const auto parsed = Decimal64::parse(text);
    if (parsed.coefficient() == 0) {
      return std::nullopt;
    }
    return parsed;
  }

  /**
   * Normalizes KIS's recent executions, newest first as KIS sends them.
   *
   * KIS reports an execution's time as `HHMMSS` with no date, so the KST calendar
   * day is supplied from the clock. A print is intraday by construction, so this is
   * the day it happened on.
   */
  static Result<std::vector<TradePrint>> parse_trades(
    InstrumentId instrument,
    Result<network::HttpResponse> result,
    int count
  ) {
    static_cast<void>(instrument);
    auto document = market_document(std::move(result));
    if (!document) {
      return Result<std::vector<TradePrint>>(std::move(document.error()));
    }
    try {
      const auto rows = document.value().if_contains("output");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: output");
      }
      Date today{};
      std::string ignored;
      korea_calendar(std::chrono::system_clock::now(), today, ignored);
      const auto date = compact_date(today);

      std::vector<TradePrint> prints;
      prints.reserve(rows->as_array().size());
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("execution entry is not an object");
        }
        const auto& row = value.as_object();
        const auto time = broker_detail::json_string(row, "stck_cntg_hour");
        // KIS pads a short window with blank rows rather than shortening it.
        if (time.empty()) {
          continue;
        }
        TradePrint print;
        print.price_ = Decimal64::parse(broker_detail::json_string(row, "stck_prpr"));
        print.quantity_ = Decimal64::parse(broker_detail::json_string(row, "cntg_vol"));
        print.currency_ = Currency::krw();
        print.exchange_time_ = korea_timestamp(date, time);
        if (!print.exchange_time_) {
          throw std::runtime_error("invalid execution time");
        }
        prints.push_back(std::move(print));
        if (count > 0 && static_cast<int>(prints.size()) >= count) {
          break;
        }
      }
      return Result<std::vector<TradePrint>>(std::move(prints));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_execution_response";
      error.message_ = exception.what();
      return Result<std::vector<TradePrint>>(std::move(error));
    }
  }

  /** Validates the instrument and requests its book. */
  void start_depth(InstrumentId instrument, DepthHandler completion) {
    if (instrument.country_ != Country::KR || !valid_domestic_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS order book covers Korean equities addressed by product number";
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
          request.target_ =
            "/uapi/domestic-stock/v1/quotations/inquire-asking-price-exp-ccn"
            "?FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" +
            broker_detail::query_encode(instrument.symbol_);
          request.headers_ = {
            {"authorization", "Bearer " + self->token_.access_token_},
            {"appkey", self->config_.key()},
            {"appsecret", self->config_.secret()},
            {"tr_id", "FHKST01010200"},
            {"custtype", "P"},
            {"content-type", "application/json; charset=utf-8"},
          };
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
   * Normalizes one KIS order book.
   *
   * KIS numbers its levels into flat fields — `askp1` through `askp10` with
   * `askp_rsqn1` through `askp_rsqn10`, and the same for bids — rather than sending
   * arrays. A level whose price is absent, blank, or zero is a level that is not
   * resting, and is skipped rather than recorded as an offer at nothing.
   *
   * The after-hours totals KIS also reports (`ovtm_total_askp_rsqn` and its
   * siblings) and its expected-execution columns are deliberately not normalized:
   * Toss publishes no counterpart, so they stay reachable on this client.
   */
  static Result<DepthSnapshot> parse_depth(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      return Result<DepthSnapshot>(std::move(result.error()));
    }
    if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      return Result<DepthSnapshot>(broker_detail::http_error(result.value()));
    }
    BrokerError parse_error;
    auto document = broker_detail::parse_object(result.value().body(), parse_error);
    if (!document) {
      return Result<DepthSnapshot>(std::move(parse_error));
    }
    try {
      if (auto declared = kis_declared_error(*document)) {
        return Result<DepthSnapshot>(std::move(*declared));
      }
      const auto output = document->if_contains("output1");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output1");
      }
      const auto& row = output->as_object();

      DepthSnapshot book;
      book.instrument_ = std::move(instrument);
      book.currency_ = Currency::krw();
      for (int level = 1; level <= depth_levels_; ++level) {
        append_level(row, "askp", "askp_rsqn", level, book.asks_);
        append_level(row, "bidp", "bidp_rsqn", level, book.bids_);
      }
      order_depth(book);
      if (const auto hour = row.if_contains("aspr_acpt_hour"); hour && hour->is_string()) {
        const std::string time(hour->as_string());
        Date today{};
        std::string ignored;
        korea_calendar(std::chrono::system_clock::now(), today, ignored);
        book.exchange_time_ = korea_timestamp(compact_date(today), time);
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

  /** How many resting levels KIS publishes on each side. */
  static constexpr int depth_levels_ = 10;

  /** Reads one numbered level, skipping a level that is not resting. */
  static void append_level(
    const json::object& row,
    std::string_view price_prefix,
    std::string_view quantity_prefix,
    int level,
    std::vector<DepthLevel>& levels
  ) {
    const auto suffix = std::to_string(level);
    const auto price = row.if_contains(std::string(price_prefix) + suffix);
    const auto quantity = row.if_contains(std::string(quantity_prefix) + suffix);
    if (!price || !price->is_string() || !quantity || !quantity->is_string()) {
      return;
    }
    const std::string price_text(price->as_string());
    const std::string quantity_text(quantity->as_string());
    if (price_text.empty() || quantity_text.empty()) {
      return;
    }
    const auto value = Decimal64::parse(price_text);
    if (value.coefficient() == 0) {
      return;
    }
    levels.push_back(DepthLevel{value, Decimal64::parse(quantity_text)});
  }

  void start_quote(InstrumentId instrument, QuoteHandler completion) {
    if (instrument.country_ != Country::KR || !broker_detail::valid_symbol(instrument.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
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

  void start_buying_power(Currency currency, KisAccount account, BuyingPowerHandler completion) {
    auto invalid = validate_account(account);
    if (!invalid && !(currency == Currency::krw())) {
      // The domestic inquiry answers in won. Answering a dollar request with a won figure under a
      // dollar label is the silent downgrade Decision 6 forbids, and KIS's foreign-currency
      // buying power is a different endpoint answering a different question.
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "unsupported_currency";
      error.message_ =
        "the KIS domestic buying-power inquiry answers in KRW only, not " + currency.str();
      invalid = std::move(error);
    }
    if (invalid) {
      complete_buying_power(std::move(completion), Result<BuyingPower>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), account = std::move(account),
         completion = std::move(completion)](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->complete_buying_power(
              std::move(completion), Result<BuyingPower>(std::move(*token_error))
            );
            return;
          }
          self->request_buying_power(std::move(account), std::move(completion));
        }
      )
    );
  }

  void request_buying_power(KisAccount account, BuyingPowerHandler completion) {
    http_->async_request(
      buying_power_request(config_, token_.access_token_, account),
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

  /**
   * Builds the account-level buying-power request.
   *
   * **`PDNO` and `ORD_UNPR` are deliberately empty**, which is how KIS is asked for the amount
   * without a quantity: "PDNO, ORD_UNPR 공란 입력 시, 매수수량 없이 매수금액만 조회됨". Both are
   * marked required, so they are sent as empty values rather than omitted.
   *
   * `ORD_DVSN` is `01`, market. The division only shapes the *quantity* answer, through the
   * instrument's margin rate, and no quantity is being asked for here -- but the field is required,
   * and `01` is the one KIS's own documentation tells a caller to use when the answer must reflect
   * margin rather than ignore it.
   *
   * `CMA_EVLU_AMT_ICLD_YN` and `OVRS_ICLD_YN` are both `N`, and that is a decision rather than a
   * default. Including a CMA valuation or overseas balances would make the figure larger than the
   * won an order can actually spend, and a buying-power check exists to be conservative. A caller
   * wanting the wider figure asks this client directly.
   */
  static network::HttpRequest buying_power_request(
    const KisConfig& config,
    std::string_view access_token,
    const KisAccount& account
  ) {
    network::HttpRequest request;
    request.group_ = network::RateLimitGroup::ACCOUNT;
    request.method_ = http::verb::get;
    request.target_ =
      "/uapi/domestic-stock/v1/trading/inquire-psbl-order"
      "?CANO=" +
      account.account_number_ + "&ACNT_PRDT_CD=" + account.product_code_ +
      "&PDNO=&ORD_UNPR=&ORD_DVSN=01&CMA_EVLU_AMT_ICLD_YN=N&OVRS_ICLD_YN=N";
    request.headers_ = {
      {"authorization", "Bearer " + std::string(access_token)},
      {"appkey", config.key()},
      {"appsecret", config.secret()},
      {"tr_id", config.is_demo_ ? "VTTC8908R" : "TTTC8908R"},
      {"custtype", "P"},
      {"content-type", "application/json; charset=utf-8"},
    };
    return request;
  }

  /**
   * Normalizes KIS's buying power.
   *
   * **`nrcvb_buy_amt` is the field, not `max_buy_amt`.** The 미수-free amount is what Toss's
   * `cashBuyingPower` means -- both answer "what can be bought without credit" -- while the maximum
   * includes 미수 and means something the other broker never reports. Normalizing the larger number
   * would make one library type mean cash at one broker and cash-plus-credit at the other, which is
   * exactly what Decision 2b exists to prevent, and it would do it in the direction that permits
   * more.
   */
  static Result<BuyingPower> parse_buying_power(Result<network::HttpResponse> response) {
    auto document = market_document(std::move(response));
    if (!document) {
      return Result<BuyingPower>(std::move(document.error()));
    }
    try {
      const auto output = document.value().if_contains("output");
      if (!output || !output->is_object()) {
        throw std::runtime_error("missing object field: output");
      }
      BuyingPower power;
      power.cash_ = Money{
        Decimal64::parse(broker_detail::json_string(output->as_object(), "nrcvb_buy_amt")),
        Currency::krw(),
      };
      // KIS states no timestamp for the figure, so the honest stamp is when it arrived.
      power.received_at_ = std::chrono::system_clock::now();
      return Result<BuyingPower>(std::move(power));
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_buying_power_response";
      error.message_ = exception.what();
      return Result<BuyingPower>(std::move(error));
    }
  }

  void complete_buying_power(BuyingPowerHandler handler, Result<BuyingPower> result) {
    deliver(std::move(handler), std::move(result));
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
    // Every other request waits behind a token refresh, including a cancellation.
    request.group_ = network::RateLimitGroup::AUTH;
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

    if (failure) {
      // The waiters asked for something else, and none of their requests was sent. See the same
      // marking in the Toss client: an unmarked authentication failure reads to a runner as the
      // broker refusing an order it never received, spending rate-limit budget on nothing.
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
    const char* market = "UN";
    if (instrument.venue_ == Venue::KRX) {
      market = "J";
    } else if (instrument.venue_ == Venue::NXT) {
      market = "NX";
    }

    network::HttpRequest request;
    // Quote polling is the traffic a reserve exists to hold back.
    request.group_ = network::RateLimitGroup::MARKET_DATA;
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
    // Holdings rank above polling but may not spend the trading reserve.
    request.group_ = network::RateLimitGroup::ACCOUNT;
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

  /** Mutable state shared across a paginated modifiable-order inquiry. */
  struct OpenOrderOperation {
    KisAccount account_;
    KisOpenOrderPage page_;
    std::string foreign_cursor_;
    std::string domestic_cursor_;
    std::size_t page_count_{0};
    OpenOrderHandler completion_;

    /** Captures the selected account and final completion handler. */
    OpenOrderOperation(KisAccount selected_account, OpenOrderHandler selected_completion)
    : account_(std::move(selected_account)), completion_(std::move(selected_completion)) {}
  };

  /** Mutable state shared across a paginated order-history inquiry. */
  struct OrderHistoryOperation {
    KisAccount account_;
    KisHistoryRange range_;
    OrderPage page_;
    std::string foreign_cursor_;
    std::string domestic_cursor_;
    std::size_t page_count_{0};
    OrderPageHandler completion_;

    /** Captures the inquiry inputs and final completion handler. */
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
    error.reached_broker_ = false;
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
    error.reached_broker_ = false;
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
    error.reached_broker_ = false;
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
   * One row of KIS's `ORD_DVSN` table: a wire code and the three facts it means.
   *
   * `ORD_DVSN` is a product, not a name. `16` is a best-limit order that is
   * filled-or-killed in the regular session; `45` is a best-limit order that is
   * immediate-or-cancel in the after-market. Holding the table as rows of the
   * product is what lets an adapter answer "which code is this combination" and
   * "which combination is this code" from one transcription, in both
   * directions, without either being derived from the other.
   *
   * `venues_` is a bitmask because the three venues publish *different* code
   * sets for the same combination set: the sheet lists `01` (시장가) under
   * `[KRX]` and `[SOR]` and not under `[NXT]`, and lists `27`-`29` (GTP) under
   * `[NXT]` alone. A combination is not "supported by KIS"; it is supported at a
   * venue.
   */
  struct OrderShapeRow {
    const char* code_;
    OrderType type_;
    TimeInForce time_in_force_;
    OrderSession session_;
    unsigned venues_;
    bool requestable_;
  };

  static constexpr unsigned kKrx = 1u << 0;
  static constexpr unsigned kNxt = 1u << 1;
  static constexpr unsigned kSor = 1u << 2;

  /**
   * KIS's `ORD_DVSN` table, transcribed from the API workbook dated `20260916`.
   *
   * Two sheets contribute and they do not agree, which is why both are named
   * here rather than one being trusted for both directions:
   *
   * - `주식주문(현금)` is the **request** table. It carries `41`-`47` (KRX
   *   애프터마켓) and `27`-`29` (NXT GTP), and does *not* carry `07`.
   * - `주식정정취소가능주문조회` is a **response** table. It carries `07`
   *   (시간외 단일가) and carries neither `41`-`47` nor `27`-`29`.
   *
   * So the set this adapter must *read* is the union and the set it may *send*
   * is the request sheet's alone. `requestable_` is that distinction: a row with
   * `false` is normalized on a response and refused on a request, because
   * sending a code the order sheet does not publish is guessing.
   *
   * A combination absent from this table is refused before any network I/O, and
   * a code absent from it reads as `UNKNOWN` with the raw code preserved. Neither
   * is approximated by a neighbouring value (Decision 6).
   */
  static constexpr std::array<OrderShapeRow, 28> kOrderShapes{{
    // code  type                       condition                         session
    {"00", OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"01", OrderType::MARKET,            TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx |        kSor, true},
    {"02", OrderType::CONDITIONAL_LIMIT, TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx,               true},
    {"03", OrderType::BEST_LIMIT,        TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"04", OrderType::PRIMARY_LIMIT,     TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"05", OrderType::CLOSING_PRICE,     TimeInForce::DAY,                 OrderSession::PRE_MARKET,          kKrx,               true},
    {"06", OrderType::CLOSING_PRICE,     TimeInForce::DAY,                 OrderSession::AFTER_HOURS,         kKrx,               true},
    // Response-only: the order sheet does not publish 07.
    {"07", OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::AFTER_HOURS_AUCTION, kKrx,               false},
    {"11", OrderType::LIMIT,             TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"12", OrderType::LIMIT,             TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"13", OrderType::MARKET,            TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"14", OrderType::MARKET,            TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"15", OrderType::BEST_LIMIT,        TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"16", OrderType::BEST_LIMIT,        TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             kKrx | kNxt | kSor, true},
    {"21", OrderType::MID_PRICE,         TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx | kNxt,        true},
    {"22", OrderType::STOP_LIMIT,        TimeInForce::DAY,                 OrderSession::REGULAR,             kKrx | kNxt,        false},
    {"23", OrderType::MID_PRICE,         TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             kKrx | kNxt,        true},
    {"24", OrderType::MID_PRICE,         TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             kKrx | kNxt,        true},
    {"27", OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::GTP,                 kNxt,               true},
    {"28", OrderType::BEST_LIMIT,        TimeInForce::DAY,                 OrderSession::GTP,                 kNxt,               true},
    {"29", OrderType::PRIMARY_LIMIT,     TimeInForce::DAY,                 OrderSession::GTP,                 kNxt,               true},
    {"41", OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::AFTER_MARKET,        kKrx,               true},
    {"42", OrderType::LIMIT,             TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::AFTER_MARKET,        kKrx,               true},
    {"43", OrderType::LIMIT,             TimeInForce::FILL_OR_KILL,        OrderSession::AFTER_MARKET,        kKrx,               true},
    {"44", OrderType::BEST_LIMIT,        TimeInForce::DAY,                 OrderSession::AFTER_MARKET,        kKrx,               true},
    {"45", OrderType::BEST_LIMIT,        TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::AFTER_MARKET,        kKrx,               true},
    {"46", OrderType::BEST_LIMIT,        TimeInForce::FILL_OR_KILL,        OrderSession::AFTER_MARKET,        kKrx,               true},
    {"47", OrderType::PRIMARY_LIMIT,     TimeInForce::DAY,                 OrderSession::AFTER_MARKET,        kKrx,               true},
  }};

  /** Returns the venue bit an `EXCG_ID_DVSN_CD` value stands for. */
  static constexpr unsigned venue_bit(Venue venue) {
    switch (venue) {
      case Venue::KRX:
        return kKrx;
      case Venue::NXT:
        return kNxt;
      // The combined book is routed across both venues, which is what KIS calls
      // SOR. See `exchange_code` for why this is an explicit value rather than
      // an omitted field.
      case Venue::COMBINED:
        return kSor;
      // Nothing was asked for, so nothing is sent, and the sheet says an
      // omitted `EXCG_ID_DVSN_CD` proceeds as KRX. The code set checked is
      // therefore KRX's -- the one the order will actually meet.
      case Venue::UNSPECIFIED:
        return kKrx;
    }
    return kKrx;
  }

  /**
   * Maps a normalized order shape to KIS's `ORD_DVSN` code for a venue.
   *
   * A combination this library has no code for, or has a code for at another
   * venue, is rejected with `INVALID_ARGUMENT` before any token fetch or socket
   * write. It is never sent under a neighbouring code: `ai/order-core` settled
   * that a condition the caller stated is honoured or refused, and the failure
   * this replaces was silent — a market order bound for NXT was sent as `01`,
   * which the sheet lists under `[KRX]` and `[SOR]` and not under `[NXT]`.
   */
  static std::optional<BrokerError> order_division(
    OrderType type,
    TimeInForce time_in_force,
    OrderSession session,
    Venue venue,
    std::string& division
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "unsupported_order";

    const auto* matched = static_cast<const OrderShapeRow*>(nullptr);
    for (const auto& row : kOrderShapes) {
      if (row.type_ == type && row.time_in_force_ == time_in_force && row.session_ == session) {
        matched = &row;
        break;
      }
    }
    if (matched == nullptr) {
      error.message_ =
        std::string("KIS publishes no order code for this combination of price basis, ") +
        "validity condition and session";
      return error;
    }
    if (!matched->requestable_) {
      error.message_ =
        std::string("KIS reports order code ") + matched->code_ +
        " on an inquiry but does not publish it as a placeable order";
      return error;
    }
    if ((matched->venues_ & venue_bit(venue)) == 0) {
      error.message_ =
        std::string("KIS order code ") + matched->code_ + " is not offered at " +
        exchange_code(venue) + "; the venue publishes its own set of order codes";
      return error;
    }
    division = matched->code_;
    return std::nullopt;
  }

  /**
   * Returns whether a symbol has one of KIS's domestic product-number forms.
   *
   * KIS documents `PDNO` as a six-character stock code, or seven characters for
   * an ETN. Both lengths are accepted so an ETN order is not rejected locally
   * for a length the broker itself documents.
   */
  static bool valid_domestic_symbol(std::string_view symbol) {
    if (symbol.size() != 6 && symbol.size() != 7) {
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

  /**
   * Returns whether an order reference can address a KIS order for amendment.
   *
   * The order number is always required and always ten digits. The branch
   * number may be empty: KIS's own documented `order-rvsecncl` request example
   * sends `KRX_FWDG_ORD_ORGNO` blank, so requiring it would reject a usage the
   * broker publishes. When it is supplied it must be the documented five
   * digits, which is what an acknowledgement or an open-order row returns.
   */
  static bool valid_order_reference(const KisOrderRef& order) {
    if (!broker_detail::decimal_digits(order.order_number_, 10)) {
      return false;
    }
    return order.branch_number_.empty() ||
      broker_detail::decimal_digits(order.branch_number_, 5);
  }

  /** Returns an error when the order cannot be represented as a KIS cash order. */
  static std::optional<BrokerError> validate_order(const OrderRequest& order, std::string& division) {
    if (auto error = unsupported_client_order_id(order)) {
      return error;
    }
    if (order.instrument_.country_ != Country::KR) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ = "KIS cash orders currently support Korean equities only";
      return error;
    }
    if (!valid_domestic_symbol(order.instrument_.symbol_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ =
        "KIS domestic order symbol must contain six letters or digits, or seven for an ETN";
      return error;
    }
    if (auto error = validate_quantity(order.quantity_)) {
      return error;
    }
    if (auto error = validate_price(order.type_, order.price_)) {
      return error;
    }
    return order_division(
      order.type_, order.time_in_force_, order.session_, order.instrument_.venue_, division
    );
  }

  /**
   * Returns KIS's `EXCG_ID_DVSN_CD` value for a normalized venue, or `""` to
   * omit the field.
   *
   * `Venue::COMBINED` maps to `SOR`. It used to map to `""`, and the workbook
   * says an omitted `EXCG_ID_DVSN_CD` "미입력시 KRX로 진행" -- so a caller who
   * asked for the combined book was given KRX and not told, which is the silent
   * downgrade Decision 6 forbids. `COMBINED` is the *default* `venue_` of an
   * `InstrumentId`, so this was the common path rather than an unusual one.
   *
   * `Venue::UNSPECIFIED` still omits the field, and that is not the same thing:
   * it means the caller expressed no venue, so the broker's documented default
   * is the honest outcome rather than a substitution for something asked for.
   *
   * Note the sheet's restriction, which this adapter does not enforce because
   * the broker does: 모의투자는 KRX만 가능 -- a demo account accepts KRX alone.
   */
  static const char* exchange_code(Venue venue) {
    switch (venue) {
      case Venue::KRX:
        return "KRX";
      case Venue::NXT:
        return "NXT";
      case Venue::COMBINED:
        return "SOR";
      case Venue::UNSPECIFIED:
        return "";
    }
    return "";
  }

  /** Builds the authenticated KIS cash-order POST request. */
  static network::HttpRequest make_place_order_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    const OrderRequest& order,
    std::string_view division
  ) {
    json::object body{
      {"CANO", account.account_number_},
      {"ACNT_PRDT_CD", account.product_code_},
      {"PDNO", order.instrument_.symbol_},
      {"ORD_DVSN", std::string(division)},
      {"ORD_QTY", order.quantity_.str()},
      {"ORD_UNPR", order.price_ ? order.price_->str() : std::string("0")},
    };
    if (const auto* exchange = exchange_code(order.instrument_.venue_); *exchange != '\0') {
      body.emplace("EXCG_ID_DVSN_CD", exchange);
    }

    const bool buy = order.side_ == OrderSide::BUY;
    network::HttpRequest request;
    // An order is what the reserve is held for.
    request.group_ = network::RateLimitGroup::TRADING;
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

  /** Builds the authenticated KIS modification or cancellation POST request. */
  static network::HttpRequest make_amend_order_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    const KisAmendment& amendment,
    bool cancel,
    std::string_view division
  ) {
    json::object body{
      {"CANO", account.account_number_},
      {"ACNT_PRDT_CD", account.product_code_},
      {"KRX_FWDG_ORD_ORGNO", amendment.order_.branch_number_},
      {"ORGN_ODNO", amendment.order_.order_number_},
      {"ORD_DVSN", std::string(division)},
      {"RVSE_CNCL_DVSN_CD", cancel ? "02" : "01"},
      {"ORD_QTY", amendment.quantity_.str()},
      {"ORD_UNPR", amendment.price_ ? amendment.price_->str() : std::string("0")},
      {"QTY_ALL_ORD_YN", amendment.all_remaining_ ? "Y" : "N"},
    };

    network::HttpRequest request;
    // A modification and a cancellation share the order endpoint and its priority.
    request.group_ = network::RateLimitGroup::TRADING;
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

    // KIS reports a rate limit in the response body rather than as HTTP 429 with
    // a Retry-After header, so it has to be recognized here or it reaches the
    // caller as an ordinary protocol failure.
    if (kis_rate_limit_layer(error.broker_code_) != KisRateLimitLayer::NONE) {
      error.category_ = ErrorCategory::RATE_LIMIT;
    }
    return error;
  }

  /** Parses a KIS order-instruction response into its composite acknowledgement. */
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

  /** Validates and starts one KIS cash-order submission. */
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

  /** Validates and starts one KIS modification or cancellation submission. */
  void start_amend_order(
    KisAccount account,
    KisAmendment amendment,
    bool cancel,
    OrderAckHandler completion
  ) {
    std::string division;
    auto invalid = validate_account(account);
    if (!invalid && !valid_order_reference(amendment.order_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_order_reference";
      error.message_ =
        "KIS amendment requires a ten-digit order number and an empty or "
        "five-digit branch number";
      invalid = std::move(error);
    }
    if (!invalid) {
      invalid = validate_quantity(amendment.quantity_);
    }
    if (!invalid) {
      invalid = validate_price(amendment.type_, amendment.price_);
    }
    if (!invalid) {
      // The amendment carries its own condition and session. Sending `DAY`
      // here, as this did, re-shaped the order: an immediate-or-cancel order
      // amended to a new price arrived as a plain day order at that price.
      invalid = order_division(
        amendment.type_,
        amendment.time_in_force_,
        amendment.session_,
        amendment.venue_,
        division
      );
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

  /** Validates and starts a complete modifiable-order inquiry. */
  void start_open_orders(KisAccount account, OpenOrderHandler completion) {
    auto invalid = validate_account(account);
    if (!invalid && config_.is_demo_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
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

  /** Builds one page request for KIS orders that remain modifiable. */
  static network::HttpRequest make_open_orders_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view access_token,
    std::string_view foreign_cursor,
    std::string_view domestic_cursor,
    bool continuation
  ) {
    network::HttpRequest request;
    // KIS recommends this inquiry before an amendment, but does not require it: a
    // caller tracking its own working orders already holds the amendable quantity,
    // and KIS's own examples amend without re-reading it. It ranks above polling and
    // stays outside the reserve, which is held for the amendment itself.
    request.group_ = network::RateLimitGroup::ACCOUNT;
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

  /** Sends the next modifiable-order page request for `operation`. */
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

  /** Accumulates one modifiable-order page and advances its continuation. */
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

  /** Parses one KIS modifiable-order response page and its cursors. */
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
        apply_order_shape(broker_detail::json_string(row, "ord_dvsn_cd"), order);
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

  /** Validates and starts a complete KIS order-history inquiry. */
  void start_order_history(KisAccount account, KisHistoryRange range, OrderPageHandler completion) {
    auto invalid = validate_account(account);
    if (
      !invalid &&
      (!is_valid_date(range.from_) || !is_valid_date(range.to_) || range.to_ < range.from_)
    ) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
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

  /** Builds one page request for KIS daily order and execution history. */
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
    // Order history is account monitoring, not order submission.
    request.group_ = network::RateLimitGroup::ACCOUNT;
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

  /** Sends the next order-history page request for `operation`. */
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

  /** Accumulates one order-history page and advances its continuation. */
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

  /** Parses one KIS daily order-history response page and its cursors. */
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
        apply_order_shape(broker_detail::json_string(row, "ord_dvsn_cd"), order);
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

  /**
   * Writes the three normalized order facts a KIS `ORD_DVSN` code carries, and
   * preserves the code itself.
   *
   * The code set read here is the **union** of the order sheet's and the
   * inquiry sheets', because they differ: the inquiry publishes `07`
   * (시간외 단일가), which cannot be placed, and the order sheet publishes
   * `41`-`47`, which an inquiry has not been observed to return. A reader that
   * knew only the request table would report `UNKNOWN` for a code KIS itself
   * had sent.
   *
   * An unrecognized code leaves all three fields `UNKNOWN` and still reaches
   * the caller through `shape_code_`. It does not throw: both specifications
   * require a client to tolerate unknown codes, and one odd row must not fail a
   * whole listing.
   */
  static void apply_order_shape(std::string_view division, OrderSnapshot& order) {
    if (const auto code = Code<8>::try_make(division)) {
      order.shape_code_ = *code;
    }
    for (const auto& row : kOrderShapes) {
      if (division == row.code_) {
        order.type_ = row.type_;
        order.time_in_force_ = row.time_in_force_;
        order.session_ = row.session_;
        return;
      }
    }
    order.type_ = OrderType::UNKNOWN;
    order.time_in_force_ = TimeInForce::UNKNOWN;
    order.session_ = OrderSession::UNKNOWN;
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
   * Corrects this connection's pacing from a KIS rate-limit refusal.
   *
   * The two KIS limit layers call for different corrections. A ledger refusal is
   * a cap shared with every other customer, so the budget was not the problem and
   * only a wait helps. A personal refusal means this key's own allowance was
   * exceeded, and KIS reports no number to correct to, so the rate is halved.
   */
  void note_rate_limit(const BrokerError& error) {
    if (error.category_ != ErrorCategory::RATE_LIMIT) {
      return;
    }
    auto* limiter = http_->rate_limits();
    if (!limiter) {
      return;
    }

    // KIS states no allowance with a refusal, of either layer, so there is nothing
    // to correct the budget *to*. Admission is held and the refusal is counted; the
    // configured budget is left exactly as the caller set it, because reducing a
    // stated budget from a guess is the kind of silent downgrade this library
    // refuses everywhere else. A caller watching `broker_rejections_` climb is being
    // told to lower it.
    limiter->hold(
      error.retry_after_ ?
        std::chrono::duration_cast<network::RateLimitController::Duration>(*error.retry_after_) :
        std::chrono::duration_cast<network::RateLimitController::Duration>(std::chrono::seconds(1))
    );
  }

  void complete_order_ack(OrderAckHandler handler, Result<KisOrderAck> result) {
    deliver(std::move(handler), std::move(result));
  }

  /** Dispatches a modifiable-order result through its associated executor. */
  void complete_open_orders(OpenOrderHandler handler, Result<KisOpenOrderPage> result) {
    deliver(std::move(handler), std::move(result));
  }

  /** Dispatches an order-history result through its associated executor. */
  void complete_order_page(OrderPageHandler handler, Result<OrderPage> result) {
    deliver(std::move(handler), std::move(result));
  }

  void complete(QuoteHandler handler, Result<QuoteSnapshot> result) {
    deliver(std::move(handler), std::move(result));
  }

  void finish_holdings(
    const std::shared_ptr<HoldingsOperation>& operation,
    Result<HoldingsSnapshot> result
  ) {
    complete_holdings(std::move(operation->completion_), std::move(result));
  }

  void complete_holdings(HoldingsHandler handler, Result<HoldingsSnapshot> result) {
    deliver(std::move(handler), std::move(result));
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

  /** Returns static reference data for one instrument. */
  Result<Instrument> instrument(InstrumentId subject) {
    return run<Instrument>(
      [&](auto&& sink) { client_->async_instrument(std::move(subject), std::move(sink)); },
      "instrument"
    );
  }

  /**
   * Returns the trading calendar from `from` onwards, cached after the first call.
   *
   * KIS asks that its holiday endpoint be called at most once a day, so a repeat
   * call for a covered date is answered from the cache without a request.
   */
  Result<TradingCalendar> calendar(Date from, bool refresh = false) {
    return run<TradingCalendar>(
      [&](auto&& sink) { client_->async_calendar(from, refresh, std::move(sink)); },
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

  /** Returns the ten resting levels on each side of the book. */
  Result<DepthSnapshot> depth(InstrumentId instrument) {
    return run<DepthSnapshot>(
      [&](auto&& sink) { client_->async_depth(std::move(instrument), std::move(sink)); },
      "depth"
    );
  }

  /** Returns bars, walking KIS's date and time inputs until the count is met. */
  Result<BarSeries> bars(BarQuery query) {
    return run<BarSeries>(
      [&](auto&& sink) { client_->async_bars(std::move(query), std::move(sink)); },
      "bars"
    );
  }

  /** Returns all normalized holdings for the selected KIS account. */
  Result<HoldingsSnapshot> holdings(KisAccount account) {
    return run<HoldingsSnapshot>(
      [&](auto&& sink) { client_->async_holdings(std::move(account), std::move(sink)); },
      "holdings"
    );
  }

  /** Returns what the account may spend, in won. */
  Result<BuyingPower> buying_power(Currency currency, KisAccount account) {
    return run<BuyingPower>(
      [&](auto&& sink) {
        client_->async_buying_power(currency, std::move(account), std::move(sink));
      },
      "buying_power"
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
  std::shared_ptr<KisClient> client_;
};

} // namespace algotradelib::kis
