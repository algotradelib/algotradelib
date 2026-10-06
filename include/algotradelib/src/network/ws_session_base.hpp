#pragma once

/**
 * @brief Defined as 1 once a transport (network) header is included.
 *
 * The library's tests use it to check that the strategy headers include no transport.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace algotradelib {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
/** @brief Short for `boost::asio::ip::tcp`. */
using tcp = net::ip::tcp;

namespace network {

/** @brief Options for a WebSocket session (`WsSession`, `WssSession`). */
struct WebSocketSessionOptions {
  /** @brief The path of the upgrade request. */
  std::string target_{"/"};
  /** @brief Extra headers sent with the upgrade request. */
  std::map<std::string, std::string> headers_;
  /** @brief The deadline for the DNS lookup, connect and TLS handshake. */
  std::chrono::seconds operation_timeout_{30};
  /** @brief Whether to reconnect after a failure or a close not requested with `close()`. */
  bool automatic_reconnect_{false};
  /** @brief The first reconnect delay; it doubles on each attempt, plus up to 20% random jitter. */
  std::chrono::seconds reconnect_initial_{1};
  /** @brief The longest reconnect delay (before jitter). */
  std::chrono::seconds reconnect_max_{30};
};

/**
 * @brief The base of the WebSocket clients `WsSession` and `WssSession`: connecting, sending,
 *   receiving, closing and reconnecting.
 *
 * To use a session, derive from `WsSession` or `WssSession`, override `on_message` (and optionally
 * `on_open`, `on_error`, `on_closed`), create it with `std::make_shared`, and call `connect()`. All
 * handlers run on the session's strand.
 *
 * @tparam Derived The transport class (`WsSession` or `WssSession`).
 * @tparam Stream The WebSocket stream type.
 */
template <typename Derived, typename Stream>
class WebSocketSessionBase : public std::enable_shared_from_this<Derived> {
public:
  /**
   * @brief Connects asynchronously (does nothing if connected, connecting or closing), and re-enables
   *   reconnection after a `close()`.
   */
  void connect() {
    net::post(strand_, [self = shared_derived()] {
      self->stop_requested_ = false;
      self->do_connect();
    });
  }

  /**
   * @brief Queues a message; messages are sent in order once connected.
   *
   * A message whose write failed stays queued and is sent again after a reconnect.
   *
   * @param message The message.
   */
  void send(std::string message) {
    net::post(strand_, [self = shared_derived(), message = std::move(message)]() mutable {
      self->write_queue_.push_back(std::move(message));
      if (self->connected_ && !self->closing_ && !self->writing_) {
        self->do_write();
      }
    });
  }

  /**
   * @brief Closes the session asynchronously after any write in progress, and disables
   *   reconnection; `on_closed` is called when done.
   */
  void close() {
    net::post(strand_, [self = shared_derived()] {
      self->stop_requested_ = true;
      self->reconnect_timer_.cancel();
      if (self->closing_) {
        return;
      }
      self->closing_ = true;
      if (self->connecting_ && !self->connected_) {
        self->resolver_.cancel();
        if (self->ws_) {
          beast::error_code ignored;
          beast::get_lowest_layer(*self->ws_).socket().cancel(ignored);
        }
        return;
      }
      if (self->connected_ && !self->writing_) {
        self->do_close();
      } else if (!self->connected_) {
        self->closing_ = false;
        self->on_closed();
      }
    });
  }

protected:
  /**
   * @brief Constructs the session; nothing connects until `connect()`.
   * @param ioc The I/O context.
   * @param host The host.
   * @param port The port.
   * @param options The options.
   */
  WebSocketSessionBase(
    net::io_context& ioc,
    std::string host,
    std::string port,
    WebSocketSessionOptions options
  )
  : strand_(net::make_strand(ioc)), resolver_(strand_), reconnect_timer_(strand_),
    host_(std::move(host)), port_(std::move(port)), options_(std::move(options)),
    reconnect_rng_(std::random_device{}()) {}

  virtual ~WebSocketSessionBase() = default;

  /**
   * @brief Called with each complete message received.
   * @param message The message; valid only during the call.
   */
  virtual void on_message(std::string_view message) = 0;

  /** @brief Called when the session is connected (after each successful upgrade, reconnects included). */
  virtual void on_open() {}

  /**
   * @brief Called when a stage fails; the session is then torn down, and reconnects if
   *   `automatic_reconnect_` is set.
   * @param stage The stage, such as `"connect"`, `"handshake"`, `"read"` or `"write"`.
   * @param error The error.
   */
  virtual void on_error(std::string_view stage, beast::error_code error) {
    boost::ignore_unused(stage, error);
  }

  /** @brief Called when the session has closed (by `close()` or by the server). */
  virtual void on_closed() {}

  /** @brief The strand every handler runs on. */
  net::strand<net::io_context::executor_type> strand_;
  /** @brief The resolver. */
  tcp::resolver resolver_;
  /** @brief The reconnect timer. */
  net::steady_timer reconnect_timer_;
  /** @brief The stream, created by the transport class on each connect. */
  std::unique_ptr<Stream> ws_;
  /** @brief The read buffer. */
  beast::flat_buffer buffer_;
  /** @brief Messages waiting to be sent; the front one may be being written. */
  std::deque<std::string> write_queue_;
  /** @brief The host. */
  std::string host_;
  /** @brief The port. */
  std::string port_;
  /** @brief The `Host` header value (`host:port`), set on connect. */
  std::string host_header_;
  /** @brief The options. */
  WebSocketSessionOptions options_;
  /** @brief Whether the session is connected. */
  bool connected_{false};
  /** @brief Whether a connect is in progress. */
  bool connecting_{false};
  /** @brief Whether a close is in progress. */
  bool closing_{false};
  /** @brief Whether a write is in progress. */
  bool writing_{false};
  /** @brief Whether `close()` was called (no reconnection). */
  bool stop_requested_{false};
  /** @brief Reconnect attempts since the last successful connect. */
  unsigned int reconnect_attempt_{0};
  /** @brief The random source for reconnect jitter. */
  std::minstd_rand reconnect_rng_;

  /**
   * @brief Returns a shared pointer to this session as the transport type.
   * @return The pointer.
   */
  std::shared_ptr<Derived> shared_derived() {
    return std::static_pointer_cast<Derived>(this->shared_from_this());
  }

  /** @brief Starts the DNS lookup; called by the transport class's `do_connect()` once `ws_` is ready. */
  void start_resolve() {
    connecting_ = true;
    resolver_.async_resolve(
      host_, port_, beast::bind_front_handler(&WebSocketSessionBase::on_resolve, shared_derived())
    );
  }

  /** @brief Sends the upgrade request; called by the transport class's `on_connect()` once connected. */
  void start_ws_handshake() {
    beast::get_lowest_layer(*ws_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.keep_alive_pings = true;
    ws_->set_option(timeout);
    ws_->set_option(websocket::stream_base::decorator([this](websocket::request_type& request) {
      request.set(http::field::user_agent, "algotradelib");
      for (const auto& header : options_.headers_) {
        request.set(header.first, header.second);
      }
    }));
    ws_->async_handshake(
      host_header_,
      options_.target_,
      beast::bind_front_handler(&WebSocketSessionBase::on_handshake, shared_derived())
    );
  }

  /**
   * @brief Tears the session down after a failure, calls `on_error` (and `on_closed` if closing),
   *   and schedules a reconnect if configured.
   * @param stage The stage that failed.
   * @param error The error.
   */
  void fail(std::string_view stage, beast::error_code error) {
    connected_ = false;
    connecting_ = false;
    writing_ = false;
    const bool closing = closing_;
    closing_ = false;
    if (ws_) {
      beast::error_code ignored;
      beast::get_lowest_layer(*ws_).socket().close(ignored);
    }
    on_error(stage, error);
    if (closing) {
      on_closed();
    }
    schedule_reconnect();
  }

private:
  void on_resolve(beast::error_code error, tcp::resolver::results_type endpoints) {
    if (error) {
      fail("resolve", error);
      return;
    }
    beast::get_lowest_layer(*ws_).expires_after(options_.operation_timeout_);
    beast::get_lowest_layer(*ws_).async_connect(
      endpoints, beast::bind_front_handler(&Derived::on_connect, shared_derived())
    );
  }

  void on_handshake(beast::error_code error) {
    if (error) {
      fail("handshake", error);
      return;
    }
    connecting_ = false;
    connected_ = true;
    reconnect_attempt_ = 0;
    on_open();
    do_read();
    if (!write_queue_.empty()) {
      do_write();
    }
  }

  void do_write() {
    writing_ = true;
    ws_->async_write(
      net::buffer(write_queue_.front()),
      beast::bind_front_handler(&WebSocketSessionBase::on_write, shared_derived())
    );
  }

  void on_write(beast::error_code error, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    writing_ = false;
    if (error) {
      fail("write", error);
      return;
    }
    write_queue_.pop_front();
    if (closing_) {
      do_close();
    } else if (!write_queue_.empty()) {
      do_write();
    }
  }

  void do_read() {
    ws_->async_read(
      buffer_, beast::bind_front_handler(&WebSocketSessionBase::on_read, shared_derived())
    );
  }

  void on_read(beast::error_code error, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    if (error == websocket::error::closed) {
      connected_ = false;
      connecting_ = false;
      closing_ = false;
      on_closed();
      schedule_reconnect();
      return;
    }
    if (error) {
      fail("read", error);
      return;
    }
    const auto message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    on_message(message);
    do_read();
  }

  void do_close() {
    ws_->async_close(
      websocket::close_code::normal,
      beast::bind_front_handler(&WebSocketSessionBase::on_close, shared_derived())
    );
  }

  void on_close(beast::error_code error) {
    connected_ = false;
    connecting_ = false;
    closing_ = false;
    if (error) {
      on_error("close", error);
    }
    on_closed();
    if (!stop_requested_) {
      schedule_reconnect();
    }
  }

  void schedule_reconnect() {
    if (stop_requested_ || !options_.automatic_reconnect_ || closing_) {
      return;
    }
    const auto shift = std::min(reconnect_attempt_, 16U);
    const auto multiplier = static_cast<std::int64_t>(1ULL << shift);
    const auto delay = std::min(options_.reconnect_initial_ * multiplier, options_.reconnect_max_);
    ++reconnect_attempt_;
    const auto base_delay = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
    const auto jitter_window = base_delay / 5;
    const auto jitter = jitter_window.count() <= 0 ?
      std::chrono::milliseconds(0) :
      std::chrono::milliseconds(
        std::uniform_int_distribution<std::int64_t>(0, jitter_window.count())(reconnect_rng_)
      );
    reconnect_timer_.expires_after(base_delay + jitter);
    reconnect_timer_.async_wait([self = shared_derived()](beast::error_code error) {
      if (!error) {
        self->do_connect();
      }
    });
  }
};

} // namespace network
} // namespace algotradelib
