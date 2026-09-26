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
using tcp = net::ip::tcp;

namespace network {

/** Runtime options shared by plain and TLS WebSocket sessions. */
struct WebSocketSessionOptions {
  /** The path the upgrade request targets. */
  std::string target_{"/"};
  /** Extra headers to send with the handshake. */
  std::map<std::string, std::string> headers_;
  /** The deadline applied to resolve, connect and handshake. */
  std::chrono::seconds operation_timeout_{30};
  /** Whether to reconnect after an unrequested close, with bounded exponential backoff and jitter. */
  bool automatic_reconnect_{false};
  /** The first backoff delay. */
  std::chrono::seconds reconnect_initial_{1};
  /** The ceiling the backoff grows to. */
  std::chrono::seconds reconnect_max_{30};
};

/**
 * Shared CRTP state machine for plain and TLS WebSocket clients.
 *
 * `Derived` supplies transport-specific connection setup through `do_connect`
 * and `on_connect`. This base owns serialized writes, reads, graceful close,
 * and reconnect backoff.
 */
template <typename Derived, typename Stream>
class WebSocketSessionBase : public std::enable_shared_from_this<Derived> {
public:
  /** Starts or restarts the transport connection asynchronously. */
  void connect() {
    net::post(strand_, [self = shared_derived()] {
      self->stop_requested_ = false;
      self->do_connect();
    });
  }

  /** Queues one complete WebSocket message for serialized transmission. */
  void send(std::string message) {
    net::post(strand_, [self = shared_derived(), message = std::move(message)]() mutable {
      self->write_queue_.push_back(std::move(message));
      if (self->connected_ && !self->closing_ && !self->writing_) {
        self->do_write();
      }
    });
  }

  /** Requests an asynchronous graceful close and disables reconnection. */
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
  /** Constructs the shared state. Derived classes own creating `ws_` and any transport setup. */
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

  /** Receives one complete WebSocket message. */
  virtual void on_message(std::string_view message) = 0;

  /** Called after a successful WebSocket upgrade. */
  virtual void on_open() {}

  /** Called when a transport stage fails. */
  virtual void on_error(std::string_view stage, beast::error_code error) {
    boost::ignore_unused(stage, error);
  }

  /** Called after the session closes. */
  virtual void on_closed() {}

  /** The strand every handler runs on, so the session needs no lock. */
  net::strand<net::io_context::executor_type> strand_;
  /** Resolves `host_` and `port_`. */
  tcp::resolver resolver_;
  /** Times the backoff between reconnect attempts. */
  net::steady_timer reconnect_timer_;
  /** The stream, created by the derived class once its transport is ready. */
  std::unique_ptr<Stream> ws_;
  /** Where an incoming message is read into. */
  beast::flat_buffer buffer_;
  /** Messages waiting to go out; writes are serialized, so only the front is on the wire. */
  std::deque<std::string> write_queue_;
  /** The host to connect to. */
  std::string host_;
  /** The port to connect to. */
  std::string port_;
  /** The value sent as the `Host` header, which SNI and certificate verification use. */
  std::string host_header_;
  /** The options this session was constructed with. */
  WebSocketSessionOptions options_;
  /** Whether the handshake has completed and the session is usable. */
  bool connected_{false};
  /** Whether a connect attempt is in flight. */
  bool connecting_{false};
  /** Whether a close was initiated and is not yet complete. */
  bool closing_{false};
  /** Whether a write is on the wire, which is what serializes `write_queue_`. */
  bool writing_{false};
  /** Whether a caller asked to stop, which suppresses reconnection. */
  bool stop_requested_{false};
  /** How many reconnects have been attempted, which sets the backoff. */
  unsigned int reconnect_attempt_{0};
  /** Jitter source for the backoff, so many sessions do not reconnect in lockstep. */
  std::minstd_rand reconnect_rng_;

  /** Returns `this` as the derived type, for a handler that must keep the session alive. */
  std::shared_ptr<Derived> shared_derived() {
    return std::static_pointer_cast<Derived>(this->shared_from_this());
  }

  /**
   * Begins resolving the host.
   *
   * Called by `Derived::do_connect()` once `ws_` — and any transport-level setup such as SNI — is
   * ready.
   */
  void start_resolve() {
    connecting_ = true;
    resolver_.async_resolve(
      host_, port_, beast::bind_front_handler(&WebSocketSessionBase::on_resolve, shared_derived())
    );
  }

  /**
   * Sends the WebSocket upgrade request.
   *
   * Called by `Derived::on_connect()`, directly or after a TLS handshake, once `host_header_` is set
   * and the transport is ready.
   */
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

  /** Records a failure at `stage`, tears the session down, and reconnects if that is configured. */
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
