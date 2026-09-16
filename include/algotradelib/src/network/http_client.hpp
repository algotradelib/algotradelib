#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <map>
#include <string>
#include <stdexcept>

namespace algotradelib {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace network {

// https://www.boost.org/doc/libs/latest/libs/beast/example/http/client/sync-ssl/http_client_sync_ssl.cpp
class HttpClient;
// https://www.boost.org/doc/libs/latest/libs/beast/example/http/client/async-ssl/http_client_async_ssl.cpp
class AsyncHttpClient;

// using HttpResponse = http::response<http::string_body>;
// using HttpRequest  = http::request<http::string_body>;

/**
 * @brief Synchronous REST SSL client
 * 
 * Order by POST, and lookup by GET
 * 
 */
class HttpClient {
public:
	HttpClient(net::io_context& ioc, ssl::context& ctx, const std::string& host, const std::string& port)
  : ioc_(ioc), ctx_(ctx), host_(host), port_(port) {}
	virtual ~HttpClient() = default;
	
	http::response<http::string_body> get(
		const std::string& target,
    const std::map<std::string, std::string>& headers = {}
	) {
		return request(http::verb::get, target, "", headers);
	}

  http::response<http::string_body> post(
		const std::string& target,
    const std::string& body,
    const std::map<std::string, std::string>& headers = {}
	) {
		return request(http::verb::post, target, body, headers);
	}

private:
	net::io_context& ioc_;
  ssl::context& ctx_;
	std::string host_, port_;
  // std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream;
  // bool is_connected = false;

	http::response<http::string_body> request(
		http::verb method,
		const std::string& target,
		const std::string& body = "",
		const std::map<std::string, std::string>& headers = {}
	) {
		tcp::resolver resolver(ioc_);
  	beast::ssl_stream<beast::tcp_stream> stream(ioc_, ctx_);
		if(!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str())) {
			throw beast::system_error(
				static_cast<int>(::ERR_get_error()),
				net::error::get_ssl_category()
			);
  	}
		auto const results = resolver.resolve(host_, port_);
		beast::get_lowest_layer(stream).connect(results);
		stream.handshake(ssl::stream_base::client);
		// http::request<http::string_body> req{http::verb::get, target, version};
		http::request<http::string_body> req{method, target, 11}; // set version with macro later
		req.set(http::field::host, host_);
		req.set(http::field::user_agent, "algotradelib");
		for (const auto& [key, val] : headers) {
			req.set(key, val);
		}
		if (!body.empty()) {
      req.body() = body;
      req.prepare_payload();
    }
    http::write(stream, req);
		beast::flat_buffer buffer;
		http::response<http::string_body> res;
		http::read(stream, buffer, res);
		beast::error_code ec;
		stream.shutdown(ec);
		// if (res.result() != http::status::ok) {
		// 	throw std::runtime_error(
		// 		"Authentification failed " + std::to_string(static_cast<int>(res.result()))
		// 			+ ": " + std::string(res.body())
		// 	);
		// }
		return res;
		// return HttpResponse{res.result_int(), std::move(res.body())};
		// HttpResponse response;
		// response.status_code = res.result_int();
		// response.body = std::move(res.body());
		// for (const auto& field : res) {
    //   response.headers.emplace(field.name_string(), field.value());
    // }
		// return response;
	}
};

/**
 * @brief Asynchronous Rest SSL session
 * 
 */
class AsyncHttpClient : public std::enable_shared_from_this<AsyncHttpClient> {
public:
	AsyncHttpClient(net::any_io_executor& ex, ssl::context& ctx, const std::string& host, const std::string& port)
	: resolver_(ex), stream_(ex, ctx),
		host_(host), port_(port) {}
	virtual ~AsyncHttpClient() = default;
	
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
			beast::bind_front_handler(&AsyncHttpClient::on_resolve, shared_from_this())
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
      beast::bind_front_handler(&AsyncHttpClient::on_connect, shared_from_this())
		);
  }

	void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
			std::cerr << "connect: " << ec.message() << "\n";
			return;
		}
		stream_.async_handshake(
			ssl::stream_base::client,
      beast::bind_front_handler(&AsyncHttpClient::on_handshake, shared_from_this())
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
      beast::bind_front_handler(&AsyncHttpClient::on_write, shared_from_this())
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
      beast::bind_front_handler(&AsyncHttpClient::on_read, shared_from_this())
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
			beast::bind_front_handler(&AsyncHttpClient::on_shutdown, shared_from_this())
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