// Uses each dependency the package carries, so a missing or unlinkable piece fails this build or this run.
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

  // Eigen, the analytics backend: mean 0.02 over a sample deviation of 0.01.
  algotradelib::math::Vector returns(3);
  returns << 0.01, 0.02, 0.03;
  const auto sharpe = algotradelib::analytics::sharpe(returns);

  const bool ok = value.at("ok").as_int64() == 1 && std::abs(sharpe - 2.0) < 1e-12;
  std::cout << "algotradelib " << ATL_VERSION_STRING << (ok ? ": ok" : ": FAILED") << '\n';
  return ok ? 0 : 1;
}
