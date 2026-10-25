// Uses each dependency the prefix carries, so a missing or unlinkable piece fails this build or this run.
#include <algotradelib/algotradelib>
#include <algotradelib/analytics>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/json.hpp>

#include <cmath>
#include <iostream>

int main() {
  // Boost.JSON, the one compiled Boost library the package config names.
  const auto value = boost::json::parse(R"({"ok": 1})");

  // Asio through Boost::json's include directory; on Windows its socket libraries must still link.
  boost::asio::io_context io;
  boost::asio::ip::tcp::socket socket(io);
  socket.open(boost::asio::ip::tcp::v4());
  socket.close();

  // Eigen, the analytics backend.
  algotradelib::math::Vector prices(3);
  prices << 100.0, 110.0, 99.0;
  const auto returns = algotradelib::analytics::returns(prices);

  const bool ok = value.at("ok").as_int64() == 1 && std::abs(returns[1] - 0.1) < 1e-12;
  std::cout << "algotradelib " << ATL_VERSION_STRING << (ok ? ": ok" : ": FAILED") << '\n';
  return ok ? 0 : 1;
}
