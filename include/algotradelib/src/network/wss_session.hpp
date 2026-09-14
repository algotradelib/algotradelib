#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <iostream>
// #include <deque>
// #include <memory>
#include <string>
#include <stdexcept>

namespace algotradelib {
namespace network {

// wss:// -> https://www.boost.org/doc/libs/latest/libs/beast/example/websocket/client/async-ssl/websocket_client_async_ssl.cpp
class WssSession;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

/**
 * @brief Asynchronous WebSocket SSL client
 * 
 */
class WssSession : public std::enable_shared_from_this<WssSession> {
public:
	WssSession(net::io_context& ioc, ssl::context& ctx, const std::string& host, const std::string& port)
	: resolver_(net::make_strand(ioc)),
    ws_(net::make_strand(ioc), ctx),
		host_(host), port_(port) {}
	virtual ~WssSession() = default;
	
	void connect(std::string& text) {
		if(!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
			throw beast::system_error(
				static_cast<int>(::ERR_get_error()),
				net::error::get_ssl_category()
			);
			// beast::error_code ec{
      //   static_cast<int>(::ERR_get_error()),
      //   net::error::get_ssl_category()
			// };
      // std::cerr << ec.message() << "\n";
      // return;
  	}
    ws_.next_layer().set_verify_callback(ssl::host_name_verification(host_));
    resolver_.async_resolve(
      host_, port_,
      beast::bind_front_handler(
				&WssSession::on_resolve,
        shared_from_this()
			)
		);
  }

protected:
	tcp::resolver resolver_;
  websocket::stream<ssl::stream<beast::tcp_stream>> ws_;
  beast::flat_buffer buffer_;
  std::string host_, port_;
  std::string text_;

	void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
		if (ec) {
			std::cerr << "resolve: " << ec.message() << "\n";
		}
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws_).async_connect(
      results,
      beast::bind_front_handler(
				&WssSession::on_connect,
        shared_from_this()
			)
		);
  }

	void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
		if (ec) {
			std::cerr << "connect: " << ec.message() << "\n";
		}
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    host_ += ':' + std::to_string(ep.port());
    ws_.next_layer().async_handshake(
      ssl::stream_base::client,
      beast::bind_front_handler(
				&WssSession::on_ssl_handshake,
        shared_from_this()
			)
		);
  }

  void on_ssl_handshake(beast::error_code ec) {
    if (ec) {
			std::cerr << "ssl_handshake: " << ec.message() << "\n";
		}
    beast::get_lowest_layer(ws_).expires_never();
    ws_.set_option(
			websocket::stream_base::timeout::suggested(
      	beast::role_type::client
			)
		);
    ws_.set_option(websocket::stream_base::decorator(
      [](websocket::request_type& req) {
        req.set(
					http::field::user_agent,
          "algotradelib"
				);
    }));
    ws_.async_handshake(
			host_, "/",
      beast::bind_front_handler(
        &WssSession::on_handshake,
        shared_from_this()
			)
		);
  }

  void on_handshake(beast::error_code ec) {
		if (ec) {
			std::cerr << "handshake: " << ec.message() << "\n";
		}
    ws_.async_write(
      net::buffer(text_),
      beast::bind_front_handler(
				&WssSession::on_write,
        shared_from_this()
			)
		);
  }

  void on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
		if (ec) {
			std::cerr << "write: " << ec.message() << "\n";
		}
    ws_.async_read(
      buffer_,
      beast::bind_front_handler(
				&WssSession::on_read,
        shared_from_this()
			)
		);
  }

  void on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
		if (ec) {
			std::cerr << "read: " << ec.message() << "\n";
		}
    ws_.async_close(
			websocket::close_code::normal,
      beast::bind_front_handler(
				&WssSession::on_close,
        shared_from_this()
			)
		);
  }

  void on_close(beast::error_code ec) {
		if (ec) {
			std::cerr << "close: " << ec.message() << "\n";
		}
    std::cout << beast::make_printable(buffer_.data()) << std::endl;
  }
};

} // namespace network
} // namespace algotradelib