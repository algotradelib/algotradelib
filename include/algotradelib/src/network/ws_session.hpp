#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <deque>
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
using tcp = net::ip::tcp;

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
  WsSession(net::io_context& ioc, std::string host, std::string port,
            std::string target = "/",
            std::map<std::string, std::string> headers = {})
      : strand_(net::make_strand(ioc)), resolver_(strand_), ws_(strand_),
        host_(std::move(host)), port_(std::move(port)), host_header_(host_),
        target_(std::move(target)), headers_(std::move(headers)) {}

  virtual ~WsSession() = default;

  void connect() {
    net::post(strand_, [self = shared_from_this()] { self->do_connect(); });
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
      if (self->closing_) {
        return;
      }
      self->closing_ = true;
      if (self->connecting_ && !self->connected_) {
        self->resolver_.cancel();
        beast::error_code ignored;
        beast::get_lowest_layer(self->ws_).socket().cancel(ignored);
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
  net::strand<net::io_context::executor_type> strand_;
  tcp::resolver resolver_;
  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  std::deque<std::string> write_queue_;
  std::string host_;
  std::string port_;
  std::string host_header_;
  std::string target_;
  std::map<std::string, std::string> headers_;
  bool connected_{false};
  bool connecting_{false};
  bool closing_{false};
  bool writing_{false};

  void do_connect() {
    if (connected_ || connecting_ || closing_) {
      return;
    }
    connecting_ = true;
    resolver_.async_resolve(host_, port_,
      beast::bind_front_handler(&WsSession::on_resolve, shared_from_this()));
  }

  void on_resolve(beast::error_code error, tcp::resolver::results_type endpoints) {
    if (error) {
      fail("resolve", error);
      return;
    }
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws_).async_connect(endpoints,
      beast::bind_front_handler(&WsSession::on_connect, shared_from_this()));
  }

  void on_connect(beast::error_code error,
                  tcp::resolver::results_type::endpoint_type endpoint) {
    if (error) {
      fail("connect", error);
      return;
    }
    beast::get_lowest_layer(ws_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.keep_alive_pings = true;
    ws_.set_option(timeout);
    ws_.set_option(websocket::stream_base::decorator(
      [this](websocket::request_type& request) {
        request.set(http::field::user_agent, "algotradelib");
        for (const auto& header : headers_) {
          request.set(header.first, header.second);
        }
      }));
    host_header_ = host_ + ":" + std::to_string(endpoint.port());
    ws_.async_handshake(host_header_, target_,
      beast::bind_front_handler(&WsSession::on_handshake, shared_from_this()));
  }

  void on_handshake(beast::error_code error) {
    if (error) {
      fail("handshake", error);
      return;
    }
    connecting_ = false;
    connected_ = true;
    on_open();
    do_read();
    if (!write_queue_.empty()) {
      do_write();
    }
  }

  void do_write() {
    writing_ = true;
    ws_.async_write(net::buffer(write_queue_.front()),
      beast::bind_front_handler(&WsSession::on_write, shared_from_this()));
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
    ws_.async_read(buffer_,
      beast::bind_front_handler(&WsSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code error, std::size_t) {
    if (error == websocket::error::closed) {
      connected_ = false;
      connecting_ = false;
      closing_ = false;
      on_closed();
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
    ws_.async_close(websocket::close_code::normal,
      beast::bind_front_handler(&WsSession::on_close, shared_from_this()));
  }

  void on_close(beast::error_code error) {
    connected_ = false;
    connecting_ = false;
    closing_ = false;
    if (error) {
      on_error("close", error);
    }
    on_closed();
  }

  void fail(std::string_view stage, beast::error_code error) {
    connected_ = false;
    connecting_ = false;
    writing_ = false;
    const bool closing = closing_;
    closing_ = false;
    on_error(stage, error);
    if (closing) {
      on_closed();
    }
  }
};

} // namespace algotradelib::network
