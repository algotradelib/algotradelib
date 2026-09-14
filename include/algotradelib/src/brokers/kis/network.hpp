#pragma once

#include <algotradelib/network>
#include <boost/json/src.hpp>
#include <boost/json.hpp>
#include <functional>
#include <unordered_map>
#include <vector>
#include <utility>
#include <sstream>

namespace algotradelib {
namespace kis {

// class KisRestClient;
// class KisWsSession;

// namespace beast = boost::beast;
// namespace http = beast::http;
// namespace websocket = beast::websocket;
// namespace net = boost::asio;
// namespace ssl = net::ssl;
// using tcp = net::ip::tcp;
// namespace json = boost::json;

using KisRestClient = network::RestClient;

} // namespace kis
} // namespace algotradelib