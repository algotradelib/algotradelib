#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
// #include <cstdlib>
// #include <functional>
#include <iostream>
#include <memory>
#include <string>
// #include <deque>
#include <stdexcept>

namespace algotradelib {
namespace network {

// ws:// -> https://www.boost.org/doc/libs/latest/libs/beast/example/websocket/client/async/websocket_client_async.cpp
class WsSession;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

/**
 * @brief Asynchronous WebSocket client
 * 
 */
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
	WsSession(net::io_context& ioc, const std::string& host, const std::string& port)
	: resolver_(net::make_strand(ioc)),
    ws_(net::make_strand(ioc)),
		host_(host), port_(port) {}
	virtual ~WsSession() = default;
	
	void connect(const std::string& text) {
		text_ = text;
		resolver_.async_resolve(
			host_, port_,
			beast::bind_front_handler(
				&WsSession::on_resolve,
				shared_from_this()
			)
		);
	}

protected:
	tcp::resolver resolver_;
  websocket::stream<beast::tcp_stream> ws_;
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
			beast::bind_front_handler(&WsSession::on_connect, shared_from_this())
		);
	}

	void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
		if (ec) {
			std::cerr << "connect: " << ec.message() << "\n";
		}
		beast::get_lowest_layer(ws_).expires_never();
		ws_.set_option(
			websocket::stream_base::timeout::suggested(
				beast::role_type::client
		));
		ws_.set_option(websocket::stream_base::decorator(
			[](websocket::request_type& req) {
				req.set(boost::beast::http::field::user_agent, "algotradelib");
			}
		));
		host_ += ':' + std::to_string(ep.port());
		ws_.async_handshake(
			host_, "/",
			beast::bind_front_handler(
				&WsSession::on_handshake,
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
        &WsSession::on_write,
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
				&WsSession::on_read,
        shared_from_this()
			)
		);
  }

	void on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
		if (ec) {
			std::cerr << "read: " << ec.message() << "\n";
		}
    ws_.async_close(websocket::close_code::normal,
      beast::bind_front_handler(
				&WsSession::on_close,
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

class WssSession : public std::enable_shared_from_this<WssSession> {
public:
	WssSession() {}
	virtual ~WssSession() = default;
	/* public */
protected:
	// 
};

} // namespace network
} // namespace algotradelib