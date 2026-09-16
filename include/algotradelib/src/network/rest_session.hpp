#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <iostream>
#include <string>
#include <stdexcept>

namespace algotradelib {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace network {

// https://www.boost.org/doc/libs/latest/libs/beast/example/http/client/async-ssl/http_client_async_ssl.cpp
class RestSession;

/**
 * @brief Asynchronous Rest SSL session
 * 
 */
class RestSession : public std::enable_shared_from_this<RestSession> {
public:
	RestSession(net::any_io_executor& ex, ssl::context& ctx, const std::string& host, const std::string& port)
	: resolver_(ex), stream_(ex, ctx),
		host_(host), port_(port) {}
	virtual ~RestSession() = default;
	
	void connect(
		http::verb method,
		const std::string& target,
		const int& version = 11
	) {
		if(!SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str())) {
			// throw beast::system_error(
			// 	static_cast<int>(::ERR_get_error()),
			// 	net::error::get_ssl_category()
			// );
			beast::error_code ec{
				static_cast<int>(::ERR_get_error()),
				net::error::get_ssl_category()
			};
			std::cerr << ec.message() << "\n";
      return;
  	}
		stream_.set_verify_callback(ssl::host_name_verification(host_));
		req_.version(version);
		req_.method(method);
		req_.target(target);
		req_.set(http::field::host, host_);
		req_.set(http::field::user_agent, "algotradelib");
		resolver_.async_resolve(
			host_, port_,
			beast::bind_front_handler(&RestSession::on_resolve, shared_from_this())
		);
	}

protected:
  tcp::resolver resolver_;
	ssl::stream<beast::tcp_stream> stream_;
  beast::flat_buffer buffer_;
	http::request<http::string_body> req_;
	http::response<http::string_body> res_;
	bool connected_, connecting_;
  std::string host_, port_;

	void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
		if (ec) {
			std::cerr << "resolve: " << ec.message() << "\n";
			return;
		}
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(stream_).async_connect(
      results,
      beast::bind_front_handler(&RestSession::on_connect, shared_from_this())
		);
  }

	void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
			std::cerr << "connect: " << ec.message() << "\n";
			return;
		}
		stream_.async_handshake(
			ssl::stream_base::client,
      beast::bind_front_handler(&RestSession::on_handshake, shared_from_this())
		);
  }

	void on_handshake(beast::error_code ec) {
		if (ec) {
			std::cerr << "handshake: " << ec.message() << "\n";
			return;
		}
		beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
		http::async_write(
			stream_, req_,
      beast::bind_front_handler(&RestSession::on_write, shared_from_this())
		);
  }

	void on_write(beast::error_code ec, std::size_t bytes_transferred) {
		boost::ignore_unused(bytes_transferred);
    if (ec) {
      std::cerr << "write: " << ec.message() << "\n";
			return;
    }
    http::async_read(
			stream_, buffer_, res_,
      beast::bind_front_handler(&RestSession::on_read, shared_from_this())
		);
  }

	void on_read(beast::error_code ec, std::size_t bytes_transferred) {
		boost::ignore_unused(bytes_transferred);
		if (ec) {
			std::cerr << "read: " << ec.message() << "\n";
			return;
		}
		std::cout << res_ << std::endl;
		beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
    stream_.async_shutdown(
			beast::bind_front_handler(&RestSession::on_shutdown, shared_from_this())
		);
	}

	void on_shutdown(beast::error_code ec) {
		if (ec != net::ssl::error::stream_truncated) {
			std::cerr << "shutdown: " << ec.message() << "\n";
			return;
		}
	}
};

} // namespace network
} // namespace algotradelib