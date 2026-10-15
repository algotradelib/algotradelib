#pragma once

/**
 * @brief Defined as 1 once a transport (network) header is included.
 *
 * The library's tests use it to check that the strategy headers include no transport.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include "./rate_limit.hpp"

#include <algotradelib/core>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
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
/** @brief Short for `boost::asio::ip::tcp`. */
using tcp = net::ip::tcp;

namespace network {

/** @brief The HTTP response type the transport clients return. */
using HttpResponse = http::response<http::string_body>;

/**
 * @brief An HTTP request for `AsyncHttpClient`.
 *
 * A broker adapter builds it and sets `group_`, the traffic class used for rate limiting and queue
 * priority.
 */
struct HttpRequest {
  /** @brief The HTTP method. A GET may be sent again once after a connection failure; nothing else is. */
  http::verb method_{http::verb::get};
  /** @brief The request target: path and query. */
  std::string target_{"/"};
  /** @brief The request body, empty for a GET. */
  std::string body_;
  /** @brief Additional headers, in the order they are sent. */
  std::vector<std::pair<std::string, std::string>> headers_;
  /** @brief The HTTP version, `11` for HTTP/1.1. */
  int version_{11};
  /**
   * @brief The traffic class, for rate limiting and queue priority.
   *
   * The default, `REFERENCE`, has the lowest priority, so an unlabelled request never displaces an
   * order.
   */
  RateLimitGroup group_{RateLimitGroup::REFERENCE};
};

/** @brief Deadlines, size limits and pacing for an HTTP client. */
struct HttpClientOptions {
  /**
   * @brief The deadline for each of DNS lookup, connect, TLS handshake, write and read, in
   *   `AsyncHttpClient`; `HttpClient`'s blocking operations are not bounded by it.
   */
  std::chrono::seconds timeout_{30};
  /** @brief The largest response body accepted, in bytes. */
  std::size_t body_limit_{8 * 1024 * 1024};
  /**
   * @brief How many requests the queue may hold, the one being sent included (`AsyncHttpClient`); one
   *   more fails with `queue_full`, unsent.
   */
  std::size_t queue_limit_{1024};

  /**
   * @brief The rate-limit budget that paces this connection's requests, or empty (the default) for
   *   no pacing (`AsyncHttpClient` only). The caller sets it; the broker clients pass it through.
   */
  std::optional<RateLimitBudget> rate_limit_;

  /**
   * @brief How long a kept-alive connection may sit idle and still carry a request other than a GET
   *   (one second by default).
   *
   * A server closes idle connections on its own schedule (KIS after five seconds), and a request
   * written into a closed connection fails. A GET is then sent again, but any other request (which
   * may be an order) is not, so its outcome would be unknown. Such a request after a longer idle
   * period is therefore sent on a fresh connection, at the cost of a TLS handshake (once per request
   * in `AsyncHttpClient`; see its description).
   */
  std::chrono::milliseconds unreplayed_idle_limit_{1000};
};

/**
 * @brief Converts a transport error into a `BrokerError`.
 * @param stage Where it failed (such as `"connect"`), stored as the error code.
 * @param ec The error.
 * @return A `CANCELLED` error for an aborted operation, otherwise `TRANSPORT`. `reached_broker_` is
 *   left at its default; the caller sets it.
 */
inline BrokerError transport_error(std::string stage, beast::error_code ec) {
  BrokerError error;
  error.category_ = ec == net::error::operation_aborted ?
    ErrorCategory::CANCELLED :
    ErrorCategory::TRANSPORT;
  error.broker_code_ = std::move(stage);
  error.message_ = ec.message();
  return error;
}

/**
 * @brief Builds an HTTP `Host` header value.
 * @param host The host name.
 * @param port The port.
 * @return `host`, with `:port` appended unless the port is `443`.
 */
inline std::string make_host_header(const std::string& host, const std::string& port) {
  return port == "443" ? host : host + ":" + port;
}

/**
 * @brief Shuts a TLS stream down (best effort) and closes its socket.
 *
 * The TLS shutdown is a blocking call that `timeout` does not bound, so it can wait for an
 * unresponsive peer.
 *
 * @param stream The stream.
 * @param timeout Set as the stream's expiry, which does not bound the blocking shutdown.
 */
inline void shutdown_ssl_stream(beast::ssl_stream<beast::tcp_stream>& stream, std::chrono::seconds timeout) noexcept {
  beast::error_code ignored;
  beast::get_lowest_layer(stream).expires_after(timeout);
  stream.shutdown(ignored);
  beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

/**
 * @brief Cancels a TLS stream's operations and closes its socket, without waiting for the TLS
 *   shutdown.
 * @param stream The stream.
 */
inline void close_ssl_stream(beast::ssl_stream<beast::tcp_stream>& stream) noexcept {
  beast::error_code ignored;
  beast::get_lowest_layer(stream).cancel();
  beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

/**
 * @brief A blocking HTTP/1.1 client over TLS that keeps one connection to one host.
 *
 * Not paced, not thread-safe, and with no enforced deadlines: a blocking DNS lookup, connect, TLS
 * handshake, write, read, or the TLS shutdown when closing can wait indefinitely. Broker clients use `AsyncHttpClient`.
 */
class HttpClient {
public:
  /**
   * @brief Constructs a client; it connects on the first request.
   * @param ioc The I/O context.
   * @param context The TLS context; borrowed, and must outlive the client. The client installs SNI
   *   and a host-name check; for the peer to be verified, the context must enable
   *   `ssl::verify_peer` and hold trusted roots.
   * @param host The host name (also used for SNI and certificate verification).
   * @param port The port.
   * @param options The options; `rate_limit_` and `queue_limit_` are not used.
   */
  HttpClient(
    net::io_context& ioc,
    ssl::context& context,
    std::string host,
    std::string port,
    HttpClientOptions options = {}
  )
  : resolver_(ioc), context_(context), host_(std::move(host)), port_(std::move(port)),
    options_(options) {}

  /**
   * @brief Sends a GET request and waits for the response.
   * @param target The path and query.
   * @param headers Additional headers.
   * @return The response, whatever its status.
   * @throws boost::system::system_error on a transport failure (after one retry).
   */
  HttpResponse get(
    const std::string& target,
    const std::map<std::string, std::string>& headers = {}
  ) {
    return request(http::verb::get, target, "", headers);
  }

  /**
   * @brief Sends a POST request and waits for the response; never retried.
   * @param target The path and query.
   * @param body The body.
   * @param headers Additional headers.
   * @return The response, whatever its status.
   * @throws boost::system::system_error on a transport failure.
   */
  HttpResponse post(
    const std::string& target,
    const std::string& body,
    const std::map<std::string, std::string>& headers = {}
  ) {
    return request(http::verb::post, target, body, headers);
  }

  /**
   * @brief Sends a request and waits for the response; a GET that fails is sent once more.
   * @param method The HTTP method.
   * @param target The path and query.
   * @param body The body.
   * @param headers Additional headers.
   * @return The response, whatever its status.
   * @throws boost::system::system_error on a transport failure.
   */
  HttpResponse request(
    http::verb method,
    const std::string& target,
    const std::string& body = {},
    const std::map<std::string, std::string>& headers = {}
  ) {
    for (int attempt = 0;; ++attempt) {
      beast::error_code error;
      // Never replayed, so never written into a connection the server may have closed while it was
      // idle; see `HttpClientOptions::unreplayed_idle_limit_`.
      if (method != http::verb::get && idle_since_ &&
          std::chrono::steady_clock::now() - *idle_since_ > options_.unreplayed_idle_limit_) {
        // Dropped without waiting for the peer's close notification: a synchronous TLS shutdown is not
        // bounded by the stream's deadline, and a live server may never answer one.
        close_ssl_stream(*stream_);
        stream_.reset();
        buffer_.consume(buffer_.size());
        idle_since_.reset();
      }
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
        } else {
          idle_since_ = std::chrono::steady_clock::now();
        }
        return response;
      }

      disconnect();
      if (method != http::verb::get || attempt > 0) {
        throw beast::system_error(error);
      }
    }
  }

  /** @brief Closes the connection; the next request opens a new one. */
  void close() { disconnect(); }

private:
  tcp::resolver resolver_;
  ssl::context& context_;
  std::string host_;
  std::string port_;
  HttpClientOptions options_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
  beast::flat_buffer buffer_;
  // When the connection last finished a response and was kept alive; empty while there is none.
  std::optional<std::chrono::steady_clock::time_point> idle_since_;

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
    idle_since_.reset();
  }
};

/**
 * @brief An asynchronous HTTP/1.1 client over TLS that keeps one connection to one host, used by the
 *   broker clients.
 *
 * Requests are queued by priority (`HttpRequest::group_`, then arrival) and sent one at a time on a
 * strand. Each completion runs on the handler's associated executor, or on the client's strand if it
 * has none.
 *
 * A GET that fails on the connection is sent once more; any other request never is. Before a request
 * other than a GET is admitted, a connection idle longer than
 * `HttpClientOptions::unreplayed_idle_limit_` is replaced by a fresh one, once per request: if the
 * request then waits for rate-limit admission longer than that limit, it is still written on the
 * fresh connection. With a rate-limit budget, a request waits for admission, and fails with
 * `admission_deadline` if the wait would exceed the timeout.
 *
 * A failed request's `BrokerError::reached_broker_` is `false` only if no attempt at it began
 * writing (a failure while resolving, connecting or handshaking, or a refusal in the queue). `true`
 * means a write was begun, by this attempt or an earlier one (a retried GET), so the broker may have
 * received it; it does not prove it did.
 *
 * Create it with `std::make_shared`: it uses `shared_from_this`.
 */
class AsyncHttpClient : public std::enable_shared_from_this<AsyncHttpClient> {
public:
  /** @brief The completion handler type: called once with the response or an error. */
  using CompletionHandler = net::any_completion_handler<void(Result<HttpResponse>)>;

  /**
   * @brief Constructs a client; it connects on the first request.
   * @param executor The executor its strand runs on.
   * @param context The TLS context; borrowed, and must outlive the client. The client installs SNI
   *   and a host-name check; for the peer to be verified, the context must enable
   *   `ssl::verify_peer` and hold trusted roots.
   * @param host The host name (also used for SNI and certificate verification).
   * @param port The port.
   * @param options The options.
   * @throws std::invalid_argument if `options.rate_limit_` has a burst or rate below 1.
   */
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
   * @brief Returns the connection's rate limiter.
   *
   * It is not synchronized: use it on the client's strand, or while no request is in progress. A
   * completion handler bound to another executor is not on the strand.
   *
   * @return The limiter, or null if the client has no budget.
   */
  RateLimitController* rate_limits() noexcept {
    return limiter_ ? &*limiter_ : nullptr;
  }

  /**
   * @brief Returns the connection's rate limiter (read-only).
   *
   * Not synchronized: read it on the client's strand, or while no request is in progress.
   *
   * @return The limiter, or null if the client has no budget.
   */
  const RateLimitController* rate_limits() const noexcept {
    return limiter_ ? &*limiter_ : nullptr;
  }

  /**
   * @brief Returns how many times the client started a rate-limit admission wait.
   *
   * It counts waits started, not requests delayed (a request can wait more than once); for delayed
   * requests read `RateLimitStats::delayed_`. Always zero for a client without a budget. Read it on
   * the client's strand or after the event loop has stopped.
   *
   * @return The count.
   */
  std::uint64_t admission_waits() const noexcept { return admission_timer_.arms(); }

  /**
   * @brief Queues a request; completes with the response (any status) or an error.
   *
   * Errors: `queue_full` (unsent) when the queue is full; `CANCELLED` if the client is closed or the
   * admission wait would exceed the timeout (`admission_deadline`); `TRANSPORT` or `CANCELLED` from
   * the connection. See the class description for `reached_broker_`. The handler runs on its
   * associated executor, or on the client's strand if it has none.
   *
   * @tparam CompletionToken An Asio completion token.
   * @param request_value The request.
   * @param token The completion token, called with `Result<HttpResponse>`.
   * @return Whatever the completion token returns.
   */
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

  /**
   * @brief Closes the client: cancels the connection and completes every queued request with
   *   `CANCELLED`. Later requests fail at once.
   */
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
    // Whether this request already went to a fresh connection because the last one sat idle. Once is
    // enough, and it bounds the rule: a fresh connection starts its own idle clock, which with a limit
    // of zero or below would otherwise send it straight back for another.
    bool refreshed_{false};
    // Whether any attempt at this request has begun writing it to the socket.
    //
    // The honest record of what a caller may be told about the broker having seen it, and
    // deliberately not `retried_`, which counts a replay rather than a send: a GET that fails to
    // *connect* is retried without a byte having left, and reading the retry as evidence would
    // report an unsent request as one the broker may hold. Sticky once set, because a second
    // attempt cannot unsend the first.
    bool sent_{false};
    // When this request began waiting for rate-limit admission; empty while it is not waiting. Kept
    // on the request, not the client: a request inserted ahead of a waiting one is admitted on its
    // own terms, and must neither be charged the other's wait nor restart it.
    std::optional<std::chrono::steady_clock::time_point> waiting_since_;

    Pending(HttpRequest request_value, CompletionHandler completion)
    : request_(std::move(request_value)), handler_(std::move(completion)) {}
  };

  net::strand<net::any_io_executor> strand_;
  tcp::resolver resolver_;
  net::steady_timer resolve_timer_;
  // The admission timer, wrapped so that arming it is counted by construction.
  //
  // **A counter beside the timer is not enough, and that is not hypothetical.** The first version of
  // this observable incremented a member at the one call site that armed the timer, on the paced
  // branch. `pump()` returns before that site when there is no budget, so a regression arming this
  // timer on the *unpaced* branch would have delayed every request on an unmetered API while the
  // counter still read zero -- and the test asserting zero would have passed.
  //
  // Making the count a property of the timer removes the bypass rather than documenting it: there
  // is no way to arm *this* timer without `expires_after`, so there is no way to wait on it without
  // saying so. What that does not cover is a delay built on some other timer; see
  // `admission_waits()`.
  //
  // **A narrow wrapper for two call sites, not a general Asio timer substitute.** It discards the
  // completion-token return values its callers do not use, and it counts an expiry reset even when
  // no wait follows -- which is exact for the one place that arms this timer and immediately waits,
  // and would need revisiting for any other use. Changing an expiry cancels a pending wait, whose
  // handler then receives `operation_aborted`; this client's handler already returns on any error.
  class CountedTimer {
  public:
    explicit CountedTimer(net::strand<net::any_io_executor>& strand) : timer_(strand) {}

    // Arms the timer for `delay`, and counts the arm.
    template <typename Rep, typename Period>
    void expires_after(const std::chrono::duration<Rep, Period>& delay) {
      timer_.expires_after(delay);
      // Counted after the call, so this records an arm that happened rather than one attempted.
      ++arms_;
    }

    // Waits, forwarding to the underlying timer.
    template <typename Handler>
    void async_wait(Handler&& handler) {
      timer_.async_wait(std::forward<Handler>(handler));
    }

    // Cancels any outstanding wait. Not counted: a cancellation is not an admission wait.
    std::size_t cancel() { return timer_.cancel(); }

    // Returns how many times this timer has been armed.
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
  std::optional<http::request<http::string_body>> active_request_;
  std::optional<http::response_parser<http::string_body>> parser_;
  // When the connection last finished a response and was kept alive; empty while there is none.
  std::optional<std::chrono::steady_clock::time_point> idle_since_;

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

  // Returns where a request of `group` belongs in the pending queue.
  //
  // Higher priority is served first, and equal priority keeps arrival order, so
  // a queue carrying one traffic class behaves exactly as a FIFO queue does.
  //
  // The front request is skipped while it is on the wire. Reordering around an
  // in-flight request would detach it from the response being read for it, which
  // is the one thing priority must never be allowed to do.
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

  // Returns whether the front request has already been committed to the wire.
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
    // A request that is never replayed is not written into a connection the server may have closed
    // while it sat idle; see `HttpClientOptions::unreplayed_idle_limit_`. Before admission, so the
    // reconnect spends no rate-limit slot of its own.
    if (queue_.front().request_.method_ != http::verb::get && !queue_.front().refreshed_ && idle_since_ &&
        std::chrono::steady_clock::now() - *idle_since_ > options_.unreplayed_idle_limit_) {
      queue_.front().refreshed_ = true;
      disconnect();
      // The handshake is not admission delay; the wait, if any, is measured again from here.
      queue_.front().waiting_since_.reset();
      start_resolve();
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
      if (!queue_.front().waiting_since_) {
        queue_.front().waiting_since_ = std::chrono::steady_clock::now();
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

    if (auto& waiting_since = queue_.front().waiting_since_) {
      limiter_->record_delay(
        group,
        std::chrono::duration_cast<RateLimitController::Duration>(
          std::chrono::steady_clock::now() - *waiting_since
        )
      );
      waiting_since.reset();
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
        // The idle clock starts here too: a request can wait for admission on a connection that has
        // carried nothing yet, and the server's idle timeout runs all the same.
        self->idle_since_ = std::chrono::steady_clock::now();
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
          self->idle_since_ = std::chrono::steady_clock::now();
        } else {
          self->disconnect();
        }
        self->complete(std::move(pending.handler_), Result<HttpResponse>(std::move(response)));
        self->pump();
      }
    );
  }

  void fail(const char* stage, beast::error_code ec) {
    // **Asked of the request, not of the connection** (`sent_` is kept on each pending request), for
    // the reason `close()` gives: `disconnect()` resets the connection's state, and asking it afterwards
    // would answer "not in flight" for every failure -- including one that happened halfway through
    // writing an order. This is the same question from the other
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
    idle_since_.reset();
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
