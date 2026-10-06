#pragma once

#include "./ws_session_base.hpp"

namespace algotradelib::network {

/**
 * @brief A WebSocket client over plain TCP (no TLS); derive from it and override `on_message`.
 *
 * See `WebSocketSessionBase` for its use.
 */
class WsSession : public WebSocketSessionBase<WsSession, websocket::stream<beast::tcp_stream>> {
public:
  /**
   * @brief Constructs a session.
   * @param ioc The I/O context.
   * @param host The host.
   * @param port The port.
   * @param target The path of the upgrade request.
   * @param headers Extra headers sent with the upgrade request.
   */
  WsSession(
    net::io_context& ioc,
    std::string host,
    std::string port,
    std::string target = "/",
    std::map<std::string, std::string> headers = {}
  )
  : WebSocketSessionBase(
      ioc,
      std::move(host),
      std::move(port),
      WebSocketSessionOptions{std::move(target), std::move(headers)}
    ) {}

private:
  template <typename, typename>
  friend class WebSocketSessionBase;
  using Stream = websocket::stream<beast::tcp_stream>;

  void do_connect() {
    if (connected_ || connecting_ || closing_ || stop_requested_) {
      return;
    }
    ws_ = std::make_unique<Stream>(strand_);
    buffer_.consume(buffer_.size());
    start_resolve();
  }

  void on_connect(beast::error_code error, tcp::resolver::results_type::endpoint_type endpoint) {
    if (error) {
      fail("connect", error);
      return;
    }
    host_header_ = host_ + ":" + std::to_string(endpoint.port());
    start_ws_handshake();
  }
};

} // namespace algotradelib::network
