#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
// #include <iostream>
// #include <deque>
// #include <memory>
// #include <string>

namespace algotradelib {
namespace network {

class WsSession;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// 

} // namespace network
} // namespace algotradelib