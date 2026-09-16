#pragma once

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <map>
#include <string>
// #include <stdexcept>

namespace algotradelib {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace network {

// https://www.boost.org/doc/libs/latest/libs/beast/example/http/client/sync-ssl/http_client_sync_ssl.cpp
class RestClient;

// using HttpResponse = http::response<http::string_body>;
// using HttpRequest  = http::request<http::string_body>;

/**
 * @brief Synchronous REST SSL client
 * 
 * Order by POST, and lookup by GET
 * 
 */
class RestClient {
public:
	RestClient(net::io_context& ioc, ssl::context& ctx, const std::string& host, const std::string& port)
  : ioc_(ioc), ctx_(ctx), host_(host), port_(port) {}
	virtual ~RestClient() = default;
	
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

} // namespace network
} // namespace algotradelib