#pragma once

/**
 * Declares that this translation unit has reached the transport layer.
 *
 * Read by `tests/test-strategy-isolation.cpp` to assert that the strategy surface does not. A
 * strategy must name no broker and perform no I/O, and that property degrades silently: one
 * convenience include in a strategy header would couple every strategy to a vendor, and nothing
 * would fail until someone built the same source against the other broker. Declaring the layer
 * here makes the assertion an architectural statement rather than a search for a third-party
 * macro that happens to come along for the ride.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include "./rate_limit.hpp"

#include <algotradelib/core>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace network {

/** HTTP response type returned by the transport clients. */
using HttpResponse = http::response<http::string_body>;

/**
 * Value-owned HTTP request accepted by the asynchronous transport.
 *
 * `group_` is the request's traffic class, used for admission and for queue
 * priority. It is assigned by the broker adapter that builds the request, never
 * by the caller, so a strategy cannot label its own chart polling as trading
 * traffic. The default is the most restricted class, so an unlabelled request
 * can never displace an order.
 */
struct HttpRequest {
  /** The HTTP verb. A GET may be replayed once after a reconnect; a POST never is. */
  http::verb method_{http::verb::get};
  /** The request target, path and query. */
  std::string target_{"/"};
  /** The request body, empty for a GET. */
  std::string body_;
  /** Additional headers, in the order they should be sent. */
  std::vector<std::pair<std::string, std::string>> headers_;
  /** The HTTP version, `11` for HTTP/1.1. */
  int version_{11};
  /** The traffic class, set by the adapter that builds the request and never by a caller. */
  RateLimitGroup group_{RateLimitGroup::REFERENCE};
};

/** Resource and deadline limits for HTTP transports. */
struct HttpClientOptions {
  /** The deadline applied to each of DNS, connect, TLS, write and read. */
  std::chrono::seconds timeout_{30};
  /** The largest response body that will be accepted, in bytes. */
  std::size_t body_limit_{8 * 1024 * 1024};
  /** How many requests may wait behind the one on the wire before further ones are shed. */
  std::size_t queue_limit_{1024};

  /**
   * Request allowance for this connection, or empty to issue requests unpaced.
   *
   * Empty is the default so that pacing is something a broker adapter turns on
   * deliberately with a budget it can justify, rather than something every
   * transport user inherits with a number the library guessed.
   */
  std::optional<RateLimitBudget> rate_limit_;
};

/** Converts a Boost transport error into a broker-neutral error. */
inline BrokerError transport_error(std::string stage, beast::error_code ec) {
  BrokerError error;
  error.category_ = ec == net::error::operation_aborted ?
    ErrorCategory::CANCELLED :
    ErrorCategory::TRANSPORT;
  error.broker_code_ = std::move(stage);
  error.message_ = ec.message();
  return error;
}

/** Builds an HTTP Host header, omitting the default TLS port. */
inline std::string make_host_header(const std::string& host, const std::string& port) {
  return port == "443" ? host : host + ":" + port;
}

/**
 * Performs a bounded best-effort TLS shutdown before closing the TCP socket.
 *
 * This helper is used only by the explicitly blocking client.
 */
inline void shutdown_ssl_stream(beast::ssl_stream<beast::tcp_stream>& stream, std::chrono::seconds timeout) noexcept {
  beast::error_code ignored;
  beast::get_lowest_layer(stream).expires_after(timeout);
  stream.shutdown(ignored);
  beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

/** Cancels and closes a TLS stream without blocking for close notification. */
inline void close_ssl_stream(beast::ssl_stream<beast::tcp_stream>& stream) noexcept {
  beast::error_code ignored;
  beast::get_lowest_layer(stream).cancel();
  beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

/** Persistent blocking HTTP/1.1 TLS client for synchronous compatibility APIs. */
class HttpClient {
public:
  /** Constructs a client for one TLS origin. */
  HttpClient(
    net::io_context& ioc,
    ssl::context& context,
    std::string host,
    std::string port,
    HttpClientOptions options = {}
  )
  : resolver_(ioc), context_(context), host_(std::move(host)), port_(std::move(port)),
    options_(options) {}

  /** Performs a blocking GET request. */
  HttpResponse get(
    const std::string& target,
    const std::map<std::string, std::string>& headers = {}
  ) {
    return request(http::verb::get, target, "", headers);
  }

  /** Performs a blocking POST request. */
  HttpResponse post(
    const std::string& target,
    const std::string& body,
    const std::map<std::string, std::string>& headers = {}
  ) {
    return request(http::verb::post, target, body, headers);
  }

  /**
   * Performs a blocking request and retries a failed GET at most once.
   *
   * Network operations use error-code overloads. A terminal transport error is
   * converted to `beast::system_error` only after retry policy is resolved.
   */
  HttpResponse request(
    http::verb method,
    const std::string& target,
    const std::string& body = {},
    const std::map<std::string, std::string>& headers = {}
  ) {
    for (int attempt = 0;; ++attempt) {
      beast::error_code error;
      ensure_connected(error);
      if (!error) {
        http::request<http::string_body> request_value{method, target, 11};
        request_value.set(http::field::host, host_header());
        request_value.set(http::field::user_agent, "algotradelib");
        request_value.keep_alive(true);
        for (const auto& header : headers) {
          request_value.set(header.first, header.second);
        }
        request_value.body() = body;
        if (!body.empty()) {
          request_value.prepare_payload();
        }

        beast::get_lowest_layer(*stream_).expires_after(options_.timeout_);
        http::write(*stream_, request_value, error);
      }

      http::response_parser<http::string_body> parser;
      if (!error) {
        parser.body_limit(options_.body_limit_);
        http::read(*stream_, buffer_, parser, error);
      }

      if (!error) {
        auto response = parser.release();
        if (!response.keep_alive() || response.need_eof()) {
          disconnect();
        }
        return response;
      }

      disconnect();
      if (method != http::verb::get || attempt > 0) {
        throw beast::system_error(error);
      }
    }
  }

  /** Closes the persistent connection. */
  void close() { disconnect(); }

private:
  tcp::resolver resolver_;
  ssl::context& context_;
  std::string host_;
  std::string port_;
  HttpClientOptions options_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
  beast::flat_buffer buffer_;

  void ensure_connected(beast::error_code& error) {
    if (stream_ && beast::get_lowest_layer(*stream_).socket().is_open()) {
      return;
    }
    stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(resolver_.get_executor(), context_);
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host_.c_str())) {
      error = beast::error_code(
        static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()
      );
      return;
    }
    stream_->set_verify_callback(ssl::host_name_verification(host_));
    const auto endpoints = resolver_.resolve(host_, port_, error);
    if (error) {
      return;
    }
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout_);
    beast::get_lowest_layer(*stream_).connect(endpoints, error);
    if (error) {
      return;
    }
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout_);
    stream_->handshake(ssl::stream_base::client, error);
  }

  std::string host_header() const { return make_host_header(host_, port_); }

  void disconnect() noexcept {
    if (!stream_) {
      return;
    }
    shutdown_ssl_stream(*stream_, std::chrono::seconds(1));
    stream_.reset();
    buffer_.consume(buffer_.size());
  }
};

/**
 * Persistent asynchronous HTTP/1.1 TLS client for one origin.
 *
 * Requests are serialized on a strand because HTTP pipelining is not used.
 * Idempotent GET requests may be replayed once after transport failure; POST
 * requests are never replayed automatically.
 */
class AsyncHttpClient : public std::enable_shared_from_this<AsyncHttpClient> {
public:
  /** Type-erased one-shot HTTP completion handler. */
  using CompletionHandler = net::any_completion_handler<void(Result<HttpResponse>)>;

  /** Constructs a client for one TLS origin. */
  AsyncHttpClient(
    net::any_io_executor executor,
    ssl::context& context,
    std::string host,
    std::string port,
    HttpClientOptions options = {}
  )
  : strand_(net::make_strand(std::move(executor))), resolver_(strand_), resolve_timer_(strand_),
    admission_timer_(strand_), context_(context), host_(std::move(host)),
    port_(std::move(port)), options_(options) {
    if (options_.rate_limit_) {
      limiter_.emplace(*options_.rate_limit_);
    }
  }

  /**
   * Returns this connection's pacer, or null when requests are unpaced.
   *
   * Callers must use it only from this client's strand, which is where every
   * response handler already runs.
   */
  RateLimitController* rate_limits() noexcept {
    return limiter_ ? &*limiter_ : nullptr;
  }

  /** Returns this connection's pacer, or null when requests are unpaced. */
  const RateLimitController* rate_limits() const noexcept {
    return limiter_ ? &*limiter_ : nullptr;
  }

  /**
   * Returns how many times this connection armed its admission wait.
   *
   * **Arms, not requests.** `pump()` re-evaluates admission for whichever request is at the front
   * *now*, so a request that waits while a higher-priority one arrives can arm the timer more than
   * once, and one arm can cover a request that never waited on its own account. The number is exact
   * and it counts arms; a caller wanting "requests the pacer decided to delay" wants
   * `RateLimitStats::delayed_`, which is a different question with a different answer.
   *
   * **Counted on the client rather than on the pacer, because the timer is the client's.** A pacer
   * exists only when a budget was configured, so nothing on `RateLimitStats` could ever say whether
   * an *unpaced* connection waited. That gap is what made "this transport is unpaced" untestable
   * except by timing it, and timing it asserts the machine rather than the library.
   *
   * A connection with no budget must report zero here, on a fast host and a hung one alike.
   *
   * **What it covers, exactly: this admission timer.** It is not a general statement that a request
   * was dispatched promptly. The other waits this client owns are deadlines rather than gates --
   * `resolve_timer_` and the beast stream timeouts abort a request rather than hold one -- but a
   * future delay built on another timer, or on a callback simply not posted, would slow a request
   * while this still read zero. The removed wall-clock ceiling would have noticed some of those.
   * That is the coverage this observable trades for determinism, and it is stated rather than
   * implied.
   *
   * Like `rate_limits()`, callers must read this from this client's strand -- which is where every
   * response handler already runs -- or after the event loop has stopped. `arms_` is a plain
   * counter, so reading it concurrently with an arm races.
   */
  std::uint64_t admission_waits() const noexcept { return admission_timer_.arms(); }

  /** Initiates a queued request and completes with a response or error. */
  template <typename CompletionToken>
  auto async_request(HttpRequest request_value, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<HttpResponse>)>(
      [self = shared_from_this(),
       request_value = std::move(request_value)](auto&& handler) mutable {
        Pending pending(
          std::move(request_value), CompletionHandler(std::forward<decltype(handler)>(handler))
        );
        net::post(self->strand_, [self, pending = std::move(pending)]() mutable {
          self->enqueue(std::move(pending));
        });
      },
      token
    );
  }

  /** Cancels transport work and completes queued requests as cancelled. */
  void close() {
    net::post(strand_, [self = shared_from_this()] {
      self->stopped_ = true;
      self->resolver_.cancel();
      self->resolve_timer_.cancel();
      self->admission_timer_.cancel();
      self->disconnect();
      BrokerError error;
      error.category_ = ErrorCategory::CANCELLED;
      error.broker_code_ = "close";
      error.message_ = "HTTP client closed";
      // Each entry answers for itself, which is why `sent_` lives on the request rather than being
      // inferred from the connection: a caller has to be able to tell an order that provably does
      // not exist -- no reconciliation owed, no request spent -- from one that may be on the wire.
      // Reading the client's state here would answer for none of them, since `disconnect()` has
      // just cleared it.
      while (!self->queue_.empty()) {
        auto pending = std::move(self->queue_.front());
        self->queue_.pop_front();
        BrokerError reported = error;
        reported.reached_broker_ = pending.sent_;
        self->complete(std::move(pending.handler_), Result<HttpResponse>(std::move(reported)));
      }
    });
  }

private:
  enum class State {
    DISCONNECTED,
    RESOLVING,
    CONNECTING,
    HANDSHAKING,
    READY,
    WRITING,
    READING,
  };

  struct Pending {
    HttpRequest request_;
    CompletionHandler handler_;
    bool retried_{false};
    /**
     * Whether any attempt at this request has begun writing it to the socket.
     *
     * The honest record of what a caller may be told about the broker having seen it, and
     * deliberately not `retried_`, which counts a replay rather than a send: a GET that fails to
     * *connect* is retried without a byte having left, and reading the retry as evidence would
     * report an unsent request as one the broker may hold. Sticky once set, because a second
     * attempt cannot unsend the first.
     */
    bool sent_{false};

    Pending(HttpRequest request_value, CompletionHandler completion)
    : request_(std::move(request_value)), handler_(std::move(completion)) {}
  };

  net::strand<net::any_io_executor> strand_;
  tcp::resolver resolver_;
  net::steady_timer resolve_timer_;
  /**
   * The admission timer, wrapped so that arming it is counted by construction.
   *
   * **A counter beside the timer is not enough, and that is not hypothetical.** The first version of
   * this observable incremented a member at the one call site that armed the timer, on the paced
   * branch. `pump()` returns before that site when there is no budget, so a regression arming this
   * timer on the *unpaced* branch would have delayed every request on an unmetered API while the
   * counter still read zero -- and the test asserting zero would have passed.
   *
   * Making the count a property of the timer removes the bypass rather than documenting it: there
   * is no way to arm *this* timer without `expires_after`, so there is no way to wait on it without
   * saying so. What that does not cover is a delay built on some other timer; see
   * `admission_waits()`.
   *
   * **A narrow wrapper for two call sites, not a general Asio timer substitute.** It discards the
   * completion-token return values its callers do not use, and it counts an expiry reset even when
   * no wait follows -- which is exact for the one place that arms this timer and immediately waits,
   * and would need revisiting for any other use. Changing an expiry cancels a pending wait, whose
   * handler then receives `operation_aborted`; this client's handler already returns on any error.
   */
  class CountedTimer {
  public:
    explicit CountedTimer(net::strand<net::any_io_executor>& strand) : timer_(strand) {}

    /** Arms the timer for `delay`, and counts the arm. */
    template <typename Rep, typename Period>
    void expires_after(const std::chrono::duration<Rep, Period>& delay) {
      timer_.expires_after(delay);
      // Counted after the call, so this records an arm that happened rather than one attempted.
      ++arms_;
    }

    /** Waits, forwarding to the underlying timer. */
    template <typename Handler>
    void async_wait(Handler&& handler) {
      timer_.async_wait(std::forward<Handler>(handler));
    }

    /** Cancels any outstanding wait. Not counted: a cancellation is not an admission wait. */
    std::size_t cancel() { return timer_.cancel(); }

    /** Returns how many times this timer has been armed. */
    std::uint64_t arms() const noexcept { return arms_; }

  private:
    net::steady_timer timer_;
    std::uint64_t arms_{0};
  };

  CountedTimer admission_timer_;
  ssl::context& context_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
  beast::flat_buffer buffer_;
  std::string host_;
  std::string port_;
  HttpClientOptions options_;
  State state_{State::DISCONNECTED};
  bool stopped_{false};
  bool resolve_timed_out_{false};
  std::deque<Pending> queue_;
  std::optional<RateLimitController> limiter_;
  std::optional<std::chrono::steady_clock::time_point> waiting_since_;
  std::optional<http::request<http::string_body>> active_request_;
  std::optional<http::response_parser<http::string_body>> parser_;

  void enqueue(Pending pending) {
    if (stopped_) {
      BrokerError error;
      error.category_ = ErrorCategory::CANCELLED;
      // Never queued, let alone sent: the instruction reached nobody.
      error.reached_broker_ = false;
      error.broker_code_ = "enqueue";
      error.message_ = "HTTP client is closed";
      complete(std::move(pending.handler_), Result<HttpResponse>(std::move(error)));
      return;
    }
    if (queue_.size() >= options_.queue_limit_) {
      if (limiter_) {
        limiter_->record_shed(pending.request_.group_);
      }
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      // Refused at admission, so nothing was sent. `TRANSPORT` would otherwise read as a request
      // that may be on the wire, and a runner would withhold the instrument to reconcile an order
      // that was never built.
      error.reached_broker_ = false;
      error.broker_code_ = "queue_full";
      error.message_ = "HTTP request queue limit exceeded";
      complete(std::move(pending.handler_), Result<HttpResponse>(std::move(error)));
      return;
    }
    queue_.insert(insertion_point(pending.request_.group_), std::move(pending));
    pump();
  }

  /**
   * Returns where a request of `group` belongs in the pending queue.
   *
   * Higher priority is served first, and equal priority keeps arrival order, so
   * a queue carrying one traffic class behaves exactly as a FIFO queue does.
   *
   * The front request is skipped while it is on the wire. Reordering around an
   * in-flight request would detach it from the response being read for it, which
   * is the one thing priority must never be allowed to do.
   */
  std::deque<Pending>::iterator insertion_point(RateLimitGroup group) {
    auto position = queue_.begin();
    if (front_in_flight() && position != queue_.end()) {
      ++position;
    }
    const int priority = rate_limit_priority(group);
    while (position != queue_.end() &&
           rate_limit_priority(position->request_.group_) >= priority) {
      ++position;
    }
    return position;
  }

  /** Returns whether the front request has already been committed to the wire. */
  bool front_in_flight() const noexcept {
    return state_ == State::WRITING || state_ == State::READING;
  }

  void pump() {
    if (stopped_ || queue_.empty()) {
      return;
    }
    if (state_ == State::DISCONNECTED) {
      start_resolve();
      return;
    }
    if (state_ != State::READY) {
      return;
    }
    if (!limiter_) {
      start_write();
      return;
    }

    // Admission is evaluated for whichever request is at the front *now*, so a
    // trading request that arrives while a polling request is waiting is
    // reconsidered immediately rather than inheriting the wait it did not earn.
    const auto group = queue_.front().request_.group_;
    const auto delay = limiter_->delay_until(group);
    if (delay > RateLimitController::Duration::zero()) {
      // Waiting longer than the operation's own deadline would queue a request
      // only to time it out on the wire. Failing now says the same thing sooner,
      // and says it before the request is sent rather than after.
      if (delay > std::chrono::duration_cast<RateLimitController::Duration>(options_.timeout_)) {
        limiter_->record_shed(group);
        auto pending = std::move(queue_.front());
        queue_.pop_front();
        waiting_since_.reset();
        BrokerError error;
        error.category_ = ErrorCategory::CANCELLED;
        // Ordinarily nothing was sent -- admission is evaluated only while the connection is
        // `READY`, never while the front request is in flight. A GET being replayed is the
        // exception: it is admitted again, and its first attempt did reach the wire.
        error.reached_broker_ = pending.sent_;
        error.broker_code_ = "admission_deadline";
        error.message_ = "request would wait for rate-limit admission beyond its deadline";
        complete(std::move(pending.handler_), Result<HttpResponse>(std::move(error)));
        pump();
        return;
      }
      if (!waiting_since_) {
        waiting_since_ = std::chrono::steady_clock::now();
      }
      admission_timer_.expires_after(delay);
      admission_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (ec) {
          return;
        }
        self->pump();
      });
      return;
    }

    if (waiting_since_) {
      limiter_->record_delay(
        group,
        std::chrono::duration_cast<RateLimitController::Duration>(
          std::chrono::steady_clock::now() - *waiting_since_
        )
      );
      waiting_since_.reset();
    }
    admission_timer_.cancel();
    limiter_->acquire(group);
    start_write();
  }

  void start_resolve() {
    stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(strand_, context_);
    state_ = State::RESOLVING;
    resolve_timed_out_ = false;
    resolve_timer_.expires_after(options_.timeout_);
    resolve_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
      if (!ec && self->state_ == State::RESOLVING) {
        self->resolve_timed_out_ = true;
        self->resolver_.cancel();
      }
    });
    resolver_.async_resolve(
      host_,
      port_,
      [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type endpoints) {
        self->resolve_timer_.cancel();
        // `close()` completes every queued request and releases the stream, so a
        // continuation that arrives afterwards has nothing left to advance.
        if (self->stopped_ || !self->stream_) {
          return;
        }
        if (ec) {
          if (self->resolve_timed_out_) {
            ec = beast::error_code(net::error::timed_out);
          }
          self->fail("resolve", ec);
          return;
        }
        self->state_ = State::CONNECTING;
        beast::get_lowest_layer(*self->stream_).expires_after(self->options_.timeout_);
        beast::get_lowest_layer(*self->stream_)
          .async_connect(
            endpoints,
            [self](beast::error_code connect_ec, tcp::resolver::results_type::endpoint_type) {
              if (self->stopped_ || !self->stream_) {
                return;
              }
              if (connect_ec) {
                self->fail("connect", connect_ec);
                return;
              }
              self->start_handshake();
            }
          );
      }
    );
  }

  void start_handshake() {
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host_.c_str())) {
      fail(
        "sni",
        beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())
      );
      return;
    }
    stream_->set_verify_callback(ssl::host_name_verification(host_));
    state_ = State::HANDSHAKING;
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout_);
    stream_->async_handshake(
      ssl::stream_base::client, [self = shared_from_this()](beast::error_code ec) {
        if (self->stopped_ || !self->stream_) {
          return;
        }
        if (ec) {
          self->fail("tls_handshake", ec);
          return;
        }
        self->state_ = State::READY;
        self->pump();
      }
    );
  }

  void start_write() {
    // Marked here, which is the only moment this can be said truthfully: everything before is
    // resolving, connecting and handshaking, none of which puts the request anywhere.
    queue_.front().sent_ = true;
    const auto& request_value = queue_.front().request_;
    active_request_.emplace(request_value.method_, request_value.target_, request_value.version_);
    active_request_->set(http::field::host, host_header());
    active_request_->set(http::field::user_agent, "algotradelib");
    active_request_->keep_alive(true);
    for (const auto& header : request_value.headers_) {
      active_request_->set(header.first, header.second);
    }
    active_request_->body() = request_value.body_;
    if (!request_value.body_.empty()) {
      active_request_->prepare_payload();
    }

    state_ = State::WRITING;
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout_);
    http::async_write(
      *stream_, *active_request_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (self->stopped_ || self->queue_.empty()) {
          return;
        }
        if (ec) {
          self->fail("write", ec);
          return;
        }
        self->start_read();
      }
    );
  }

  void start_read() {
    parser_.emplace();
    parser_->body_limit(options_.body_limit_);
    state_ = State::READING;
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout_);
    http::async_read(
      *stream_, buffer_, *parser_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (self->stopped_ || self->queue_.empty()) {
          return;
        }
        if (ec) {
          self->fail("read", ec);
          return;
        }
        auto response = self->parser_->release();
        const bool keep_alive = response.keep_alive() && !response.need_eof();
        self->parser_.reset();
        self->active_request_.reset();
        auto pending = std::move(self->queue_.front());
        self->queue_.pop_front();
        if (keep_alive) {
          self->state_ = State::READY;
        } else {
          self->disconnect();
        }
        self->complete(std::move(pending.handler_), Result<HttpResponse>(std::move(response)));
        self->pump();
      }
    );
  }

  void fail(const char* stage, beast::error_code ec) {
    // **Read before disconnecting**, for the reason `close()` gives: `disconnect()` resets the
    // state, and asking afterwards would answer "not in flight" for every failure -- including one
    // that happened halfway through writing an order. This is the same question from the other
    // side: a resolve, connect, or handshake failure happens *before* `start_write()`, so an order
    // has certainly not been sent, and reporting it as indeterminate spends a rate-limit slot and
    // withholds the instrument to reconcile an order that does not exist.
    disconnect();
    if (queue_.empty()) {
      return;
    }
    auto& pending = queue_.front();
    if (
      pending.request_.method_ == http::verb::get && !pending.retried_ &&
      ec != net::error::operation_aborted
    ) {
      pending.retried_ = true;
      pump();
      return;
    }
    auto completed = std::move(pending);
    queue_.pop_front();
    auto error = transport_error(stage, ec);
    error.reached_broker_ = completed.sent_;
    complete(std::move(completed.handler_), Result<HttpResponse>(std::move(error)));
    pump();
  }

  void disconnect() noexcept {
    resolve_timer_.cancel();
    if (stream_) {
      close_ssl_stream(*stream_);
      stream_.reset();
    }
    buffer_.consume(buffer_.size());
    parser_.reset();
    active_request_.reset();
    state_ = State::DISCONNECTED;
  }

  std::string host_header() const { return make_host_header(host_, port_); }

  void complete(CompletionHandler handler, Result<HttpResponse> result) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }
};

} // namespace network
} // namespace algotradelib
