#pragma once

#include "./ws_session_base.hpp"

#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace algotradelib::network {

namespace ssl = net::ssl;

using WssSessionOptions = WebSocketSessionOptions;

class WssSession
: public WebSocketSessionBase<WssSession, websocket::stream<ssl::stream<beast::tcp_stream>>> {
public:
  WssSession(
    net::io_context& ioc,
    ssl::context& context,
    std::string host,
    std::string port,
    WssSessionOptions options = {}
  )
  : WebSocketSessionBase(ioc, std::move(host), std::move(port), std::move(options)),
    context_(context) {}

  WssSession(
    net::io_context& ioc,
    ssl::context& context,
    std::string host,
    std::string port,
    std::string target,
    std::map<std::string, std::string> headers = {}
  )
  : WssSession(
      ioc,
      context,
      std::move(host),
      std::move(port),
      WssSessionOptions{std::move(target), std::move(headers)}
    ) {}

private:
  template <typename, typename>
  friend class WebSocketSessionBase;
  using Stream = websocket::stream<ssl::stream<beast::tcp_stream>>;

  ssl::context& context_;

  void do_connect() {
    if (connected_ || connecting_ || closing_ || stop_requested_) {
      return;
    }
    ws_ = std::make_unique<Stream>(strand_, context_);
    buffer_.consume(buffer_.size());
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
      fail(
        "sni",
        beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())
      );
      return;
    }
    ws_->next_layer().set_verify_callback(ssl::host_name_verification(host_));
    start_resolve();
  }

  void on_connect(
    beast::error_code error,
    tcp::resolver::results_type::endpoint_type endpoint
  ) {
    if (error) {
      fail("connect", error);
      return;
    }
    host_header_ = host_ + ":" + std::to_string(endpoint.port());
    beast::get_lowest_layer(*ws_).expires_after(options_.operation_timeout);
    ws_->next_layer().async_handshake(
      ssl::stream_base::client,
      beast::bind_front_handler(&WssSession::on_ssl_handshake, shared_derived())
    );
  }

  void on_ssl_handshake(
    beast::error_code error
  ) {
    if (error) {
      fail("ssl_handshake", error);
      return;
    }
    start_ws_handshake();
  }
};

} // namespace algotradelib::network
