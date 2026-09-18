#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace algotradelib::network {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

struct WssSessionOptions {
  std::string target{"/"};
  std::map<std::string, std::string> headers;
  std::chrono::seconds operation_timeout{30};
  bool automatic_reconnect{false};
  std::chrono::seconds reconnect_initial{1};
  std::chrono::seconds reconnect_max{30};
};

class WssSession : public std::enable_shared_from_this<WssSession> {
public:
  WssSession(net::io_context& ioc, ssl::context& context, std::string host,
             std::string port, WssSessionOptions options = {})
      : strand_(net::make_strand(ioc)), resolver_(strand_),
        reconnect_timer_(strand_), context_(context), host_(std::move(host)),
        port_(std::move(port)), options_(std::move(options)) {}

  WssSession(net::io_context& ioc, ssl::context& context, std::string host,
             std::string port, std::string target,
             std::map<std::string, std::string> headers = {})
      : WssSession(ioc, context, std::move(host), std::move(port),
          WssSessionOptions{std::move(target), std::move(headers),
                            std::chrono::seconds(30), false,
                            std::chrono::seconds(1), std::chrono::seconds(30)}) {}

  virtual ~WssSession() = default;

  void connect() {
    net::post(strand_, [self = shared_from_this()] {
      self->stop_requested_ = false;
      self->do_connect();
    });
  }

  void send(std::string message) {
    net::post(strand_,
      [self = shared_from_this(), message = std::move(message)]() mutable {
        self->write_queue_.push_back(std::move(message));
        if (self->connected_ && !self->closing_ && !self->writing_) {
          self->do_write();
        }
      });
  }

  void close() {
    net::post(strand_, [self = shared_from_this()] {
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
  virtual void on_message(std::string_view message) = 0;
  virtual void on_open() {}
  virtual void on_error(std::string_view stage, beast::error_code error) {
    boost::ignore_unused(stage, error);
  }
  virtual void on_closed() {}

private:
  using Stream = websocket::stream<ssl::stream<beast::tcp_stream>>;

  net::strand<net::io_context::executor_type> strand_;
  tcp::resolver resolver_;
  net::steady_timer reconnect_timer_;
  ssl::context& context_;
  std::unique_ptr<Stream> ws_;
  beast::flat_buffer buffer_;
  std::deque<std::string> write_queue_;
  std::string host_;
  std::string port_;
  WssSessionOptions options_;
  std::string host_header_;
  bool connected_{false};
  bool connecting_{false};
  bool closing_{false};
  bool writing_{false};
  bool stop_requested_{false};
  unsigned int reconnect_attempt_{0};

  void do_connect() {
    if (connected_ || connecting_ || closing_ || stop_requested_) {
      return;
    }
    ws_ = std::make_unique<Stream>(strand_, context_);
    buffer_.consume(buffer_.size());
    connecting_ = true;
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
      fail("sni", beast::error_code(
        static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()));
      return;
    }
    ws_->next_layer().set_verify_callback(ssl::host_name_verification(host_));
    resolver_.async_resolve(host_, port_,
      beast::bind_front_handler(&WssSession::on_resolve, shared_from_this()));
  }

  void on_resolve(beast::error_code error, tcp::resolver::results_type endpoints) {
    if (error) {
      fail("resolve", error);
      return;
    }
    beast::get_lowest_layer(*ws_).expires_after(options_.operation_timeout);
    beast::get_lowest_layer(*ws_).async_connect(endpoints,
      beast::bind_front_handler(&WssSession::on_connect, shared_from_this()));
  }

  void on_connect(beast::error_code error,
                  tcp::resolver::results_type::endpoint_type endpoint) {
    if (error) {
      fail("connect", error);
      return;
    }
    host_header_ = host_ + ":" + std::to_string(endpoint.port());
    beast::get_lowest_layer(*ws_).expires_after(options_.operation_timeout);
    ws_->next_layer().async_handshake(ssl::stream_base::client,
      beast::bind_front_handler(&WssSession::on_ssl_handshake, shared_from_this()));
  }

  void on_ssl_handshake(beast::error_code error) {
    if (error) {
      fail("ssl_handshake", error);
      return;
    }
    beast::get_lowest_layer(*ws_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.keep_alive_pings = true;
    ws_->set_option(timeout);
    ws_->set_option(websocket::stream_base::decorator(
      [this](websocket::request_type& request) {
        request.set(http::field::user_agent, "algotradelib");
        for (const auto& header : options_.headers) {
          request.set(header.first, header.second);
        }
      }));
    ws_->async_handshake(host_header_, options_.target,
      beast::bind_front_handler(&WssSession::on_handshake, shared_from_this()));
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
    ws_->async_write(net::buffer(write_queue_.front()),
      beast::bind_front_handler(&WssSession::on_write, shared_from_this()));
  }

  void on_write(beast::error_code error, std::size_t) {
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
    ws_->async_read(buffer_,
      beast::bind_front_handler(&WssSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code error, std::size_t) {
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
    ws_->async_close(websocket::close_code::normal,
      beast::bind_front_handler(&WssSession::on_close, shared_from_this()));
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

  void schedule_reconnect() {
    if (stop_requested_ || !options_.automatic_reconnect || closing_) {
      return;
    }
    const auto shift = std::min(reconnect_attempt_, 16U);
    const auto multiplier = static_cast<std::int64_t>(1ULL << shift);
    const auto delay = std::min(options_.reconnect_initial * multiplier,
                                options_.reconnect_max);
    ++reconnect_attempt_;
    const auto base_delay = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
    const auto jitter_window = base_delay / 5;
    const auto seed = std::hash<std::string>{}(host_) ^ reconnect_attempt_;
    const auto jitter = std::chrono::milliseconds(
      jitter_window.count() == 0
        ? 0
        : static_cast<std::int64_t>(seed %
            static_cast<std::size_t>(jitter_window.count() + 1)));
    reconnect_timer_.expires_after(base_delay + jitter);
    reconnect_timer_.async_wait([self = shared_from_this()](beast::error_code error) {
      if (!error) {
        self->do_connect();
      }
    });
  }
};

} // namespace algotradelib::network
